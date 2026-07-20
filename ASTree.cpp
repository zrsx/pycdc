#include <cstring>
#include <cstdint>
#include <stdexcept>
#include "ASTree.h"
#include "FastStack.h"
#include "pyc_numeric.h"
#include "bytecode.h"
#include <map>
#include <algorithm>
// This must be a triple quote (''' or """), to handle interpolated string literals containing the opposite quote style.
// E.g. f'''{"interpolated "123' literal"}'''    -> valid.
// E.g. f"""{"interpolated "123' literal"}"""    -> valid.
// E.g. f'{"interpolated "123' literal"}'        -> invalid, unescaped quotes in literal.
// E.g. f'{"interpolated \"123\' literal"}'      -> invalid, f-string expression does not allow backslash.
// NOTE: Nested f-strings not supported.
#define F_STRING_QUOTE "'''"

static void append_to_chain_store(const PycRef<ASTNode>& chainStore,
        PycRef<ASTNode> item, FastStack& stack, const PycRef<ASTBlock>& curblock);

/* Use this to determine if an error occurred (and therefore, if we should
 * avoid cleaning the output tree) */
static bool cleanBuild;

/* Use this to prevent printing return keywords and newlines in lambdas. */
static bool inLambda = false;

/* Use this to keep track of whether we need to print out any docstring and
 * the list of global variables that we are using (such as inside a function). */
static bool printDocstringAndGlobals = false;

/* Use this to keep track of whether we need to print a class or module docstring */
static bool printClassDocstring = true;

// shortcut for all top/pop calls
static PycRef<ASTNode> StackPopTop(FastStack& stack)
{
    const auto node(stack.top());
    stack.pop();
    return node;
}

/* compiler generates very, VERY similar byte code for if/else statement block and if-expression
 *  statement
 *      if a: b = 1
 *      else: b = 2
 *  expression:
 *      b = 1 if a else 2
 *  (see for instance https://stackoverflow.com/a/52202007)
 *  here, try to guess if just finished else statement is part of if-expression (ternary operator)
 *  if it is, remove statements from the block and put a ternary node on top of stack
 */
static void CheckIfExpr(FastStack& stack, PycRef<ASTBlock> curblock)
{
    if (stack.empty())
        return;
    if (curblock->nodes().size() < 2)
        return;
    auto rit = curblock->nodes().crbegin();
    // the last is "else" block, the one before should be "if" (could be "for", ...)
    if ((*rit)->type() != ASTNode::NODE_BLOCK ||
        (*rit).cast<ASTBlock>()->blktype() != ASTBlock::BLK_ELSE)
        return;
    ++rit;
    if ((*rit)->type() != ASTNode::NODE_BLOCK ||
        (*rit).cast<ASTBlock>()->blktype() != ASTBlock::BLK_IF)
        return;
    auto else_expr = StackPopTop(stack);
    curblock->removeLast();
    auto if_block = curblock->nodes().back();
    auto if_expr = StackPopTop(stack);
    curblock->removeLast();
    stack.push(new ASTTernary(std::move(if_block), std::move(if_expr), std::move(else_expr)));
}

/* --------------------------------------------------------------------------
 * Python 3.11+ "zero-cost" exception support.
 *
 * Starting with 3.11, try/except/finally no longer emit SETUP_FINALLY /
 * POP_BLOCK opcodes.  Instead the protected ranges live in a side-table
 * (co_exceptiontable) and the handler code is laid out *after* the normal
 * "fall through" continuation.  To rebuild the source we pre-scan that table
 * and remember, for each user try, where its body begins/ends and where its
 * handler (the except/finally clauses) lives.
 *
 * A table entry whose handler target starts with PUSH_EXC_INFO marks a real
 * user "try" body.  Entries whose target starts with COPY/RERAISE are the
 * compiler-generated cleanup shims that simply re-raise; we use those only to
 * chain a handler back to the try that encloses it (so nested try/except can
 * recover the outer try's true extent).
 * ------------------------------------------------------------------------ */
struct ExceptRegion {
    int handler = -1;   // offset of the PUSH_EXC_INFO that begins the handler
    int try_start = 0;  // first offset of the user-visible try body
    int body_end = 0;   // offset where the protected body first stops (== start
                        // of the "fall through" continuation)
};

// Decode the opcode byte at a given offset by walking from the start.
static int opcode_at(PycRef<PycCode> code, PycModule* mod, int offset)
{
    PycBuffer buf(code->code()->value(), code->code()->length());
    int op, arg, pos = 0;
    while (!buf.atEof()) {
        int start = pos;
        bc_next(buf, mod, op, arg, pos);
        if (start == offset)
            return op;
        if (start > offset)
            break;
    }
    return Pyc::PYC_INVALID_OPCODE;
}

static std::vector<ExceptRegion> analyzeExceptRegions(PycRef<PycCode> code, PycModule* mod)
{
    std::vector<ExceptRegion> regions;
    if (mod->verCompare(3, 11) < 0)
        return regions;

    auto entries = code->exceptTableEntries();
    if (entries.empty())
        return regions;

    // Map every handler offset that begins a real user try (PUSH_EXC_INFO) to
    // the extent of its directly-protected ranges.  try_start = min fragment
    // start, body_end = min fragment end (the first offset where protection
    // stops, i.e. where the fall-through continuation begins).
    std::map<int, ExceptRegion> byHandler;
    for (const auto& entry : entries) {
        int start = std::get<0>(entry);
        int end = std::get<1>(entry);
        int target = std::get<2>(entry);
        if (opcode_at(code, mod, target) != Pyc::PUSH_EXC_INFO)
            continue;

        auto it = byHandler.find(target);
        if (it == byHandler.end()) {
            ExceptRegion r;
            r.handler = target;
            r.try_start = start;
            r.body_end = end;
            byHandler[target] = r;
        } else {
            if (start < it->second.try_start)
                it->second.try_start = start;
            if (end < it->second.body_end)
                it->second.body_end = end;
        }
    }

    for (const auto& kv : byHandler)
        regions.push_back(kv.second);

    return regions;
}


PycRef<ASTNode> BuildFromCode(PycRef<PycCode> code, PycModule* mod)
{
    PycBuffer source(code->code()->value(), code->code()->length());

    FastStack stack((mod->majorVer() == 1) ? 20 : code->stackSize());
    stackhist_t stack_hist;

    std::stack<PycRef<ASTBlock> > blocks;
    PycRef<ASTBlock> defblock = new ASTBlock(ASTBlock::BLK_MAIN);
    defblock->init();
    PycRef<ASTBlock> curblock = defblock;
    blocks.push(defblock);

    int opcode, operand;
    int curpos = 0;
    int pos = 0;
    int unpack = 0;
    bool else_pop = false;
    bool need_try = false;
    bool variable_annotations = false;

    /* Python 3.11+ zero-cost exception reconstruction (see analyzeExceptRegions).
       We recognise try-body starts and handler starts by offset while walking
       the linear byte stream, and rebuild try/except structure around them. */
    std::vector<ExceptRegion> except_regions = analyzeExceptRegions(code, mod);
    // Multiple tries can share a start offset (nested try/except).  Group them
    // by start, and within a start open the *outermost* first — the outer try
    // has the later handler offset (handlers are laid out after the body), so we
    // order by descending handler.
    std::map<int, std::vector<ExceptRegion>> zce_try_start;
    std::map<int, ExceptRegion> zce_handler;     // handler offset  -> region
    for (const auto& r : except_regions) {
        zce_try_start[r.try_start].push_back(r);
        zce_handler[r.handler] = r;
    }
    for (auto& kv : zce_try_start) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const ExceptRegion& a, const ExceptRegion& b) {
                      return a.handler > b.handler; // outermost (later handler) first
                  });
    }
    const bool use_zce = mod->verCompare(3, 11) >= 0 && !except_regions.empty();
    (void)use_zce;

    /* Containers whose try body has been emitted and are now waiting for their
       handler code (keyed by handler offset).  When we reach the handler we
       reconstruct the except/finally clauses into the stored container. */
    std::map<int, PycRef<ASTContainerBlock>> zce_pending;
    struct ZceHandlerState {
        int handler;
        PycRef<ASTContainerBlock> container;
        bool expect_as;
        bool expect_check;
        PycRef<PycString> as_name;
    };
    std::vector<ZceHandlerState> zce_handler_stack;
    /* Handler offset currently being reconstructed (-1 == none). */
    int zce_active_handler = -1;
    PycRef<ASTContainerBlock> zce_active_container;
    /* Set right after NOT_TAKEN inside a handler: the next STORE_FAST names the
       `except ... as <name>` alias rather than a normal assignment. */
    bool zce_expect_as = false;
    /* Set after PUSH_EXC_INFO: next op is CHECK_EXC_MATCH (conditional) or POP_TOP (bare except:). */
    bool zce_expect_check = false;
    /* Name of the active `except ... as <name>` alias, so we can suppress the
       compiler-generated `<name> = None; del <name>` cleanup at clause end. */
    PycRef<PycString> zce_as_name;

    while (!source.atEof()) {
#if defined(BLOCK_DEBUG) || defined(STACK_DEBUG)
        fprintf(stderr, "%-7d", pos);
    #ifdef STACK_DEBUG
        fprintf(stderr, "%-5d", (unsigned int)stack_hist.size() + 1);
    #endif
    #ifdef BLOCK_DEBUG
        for (unsigned int i = 0; i < blocks.size(); i++)
            fprintf(stderr, "    ");
        fprintf(stderr, "%s (%d)", curblock->type_str(), curblock->end());
    #endif
        fprintf(stderr, "\n");
#endif

        curpos = pos;
        bc_next(source, mod, opcode, operand, pos);

        if (need_try && opcode != Pyc::SETUP_EXCEPT_A) {
            need_try = false;

            /* Store the current stack for the except/finally statement(s) */
            stack_hist.push(stack);
            PycRef<ASTBlock> tryblock = new ASTBlock(ASTBlock::BLK_TRY, curblock->end(), true);
            blocks.push(tryblock);
            curblock = blocks.top();
        } else if (else_pop
                && opcode != Pyc::JUMP_FORWARD_A
                && opcode != Pyc::JUMP_IF_FALSE_A
                && opcode != Pyc::JUMP_IF_FALSE_OR_POP_A
                && opcode != Pyc::POP_JUMP_IF_FALSE_A
                && opcode != Pyc::POP_JUMP_FORWARD_IF_FALSE_A
                && opcode != Pyc::JUMP_IF_TRUE_A
                && opcode != Pyc::JUMP_IF_TRUE_OR_POP_A
                && opcode != Pyc::POP_JUMP_IF_TRUE_A
                && opcode != Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                && opcode != Pyc::POP_BLOCK) {
            else_pop = false;

            PycRef<ASTBlock> prev = curblock;
            while (prev->end() < pos
                    && prev->blktype() != ASTBlock::BLK_MAIN) {
                if (prev->blktype() != ASTBlock::BLK_CONTAINER) {
                    if (prev->end() == 0) {
                        break;
                    }

                    /* We want to keep the stack the same, but we need to pop
                     * a level off the history. */
                    //stack = stack_hist.top();
                    if (!stack_hist.empty())
                        stack_hist.pop();
                }
                blocks.pop();

                if (blocks.empty())
                    break;

                curblock = blocks.top();
                curblock->append(prev.cast<ASTNode>());

                prev = curblock;

                CheckIfExpr(stack, curblock);
            }
        }

        /* Python 3.11+ zero-cost exceptions.  The byte stream is laid out as
              [try body] [fall-through continuation] [handler(s)]
           with the protected ranges recorded in co_exceptiontable.  We rebuild
           the source shape in three offset-driven steps:

             (1) at a try-body start: push a container + BLK_TRY;
             (2) at the try-body end: close the try, link the container into the
                 parent block (so the continuation that follows renders after
                 it), and remember the container by its handler offset;
             (3) at the handler offset: reopen that container and reconstruct
                 the except/finally clauses into it. */
        if (use_zce) {
            // (2) End of protected body/bodies -> finish the try (innermost
            //     first) and park its container for later handler filling.
            while (curblock->blktype() == ASTBlock::BLK_TRY
                    && curblock->end() <= curpos) {
                PycRef<ASTBlock> tryblk = curblock;
                blocks.pop();
                PycRef<ASTBlock> container = blocks.top();
                int hoffset = container.cast<ASTContainerBlock>()->except();
                container->append(tryblk.cast<ASTNode>());
                blocks.pop();
                curblock = blocks.top();
                curblock->append(container.cast<ASTNode>());

                if (!stack_hist.empty()) {
                    stack = stack_hist.top();
                    stack_hist.pop();
                }
                zce_pending[hoffset] = container.cast<ASTContainerBlock>();
            }

            // (1) Start of protected body/bodies -> open a container + try for
            //     each region beginning here (outermost first).
            {
                auto tsit = zce_try_start.find(curpos);
                if (tsit != zce_try_start.end()) {
                    for (const auto& region : tsit->second) {
                        if (zce_pending.find(region.handler) != zce_pending.end())
                            continue;
                        PycRef<ASTContainerBlock> container =
                                new ASTContainerBlock(0, region.handler);
                        container->init();
                        container->setExcept(region.handler);
                        blocks.push(container.cast<ASTBlock>());
                        curblock = blocks.top();

                        stack_hist.push(stack);
                        PycRef<ASTBlock> tryblock =
                                new ASTBlock(ASTBlock::BLK_TRY, region.body_end, true);
                        blocks.push(tryblock);
                        curblock = blocks.top();
                    }
                }
            }

            // (3) Reached a handler -> reopen its container for except clauses.
            auto hit = zce_pending.find(curpos);
            if (hit != zce_pending.end()) {
                if (zce_active_handler >= 0) {
                    zce_handler_stack.push_back({zce_active_handler, zce_active_container,
                            zce_expect_as, zce_expect_check, zce_as_name});
                }
                zce_active_handler = curpos;
                zce_active_container = hit->second;
                zce_pending.erase(hit);

                /* Re-enter the container so the except/finally clauses we build
                   land inside it (it is already linked into its parent). */
                blocks.push(zce_active_container.cast<ASTBlock>());
                curblock = blocks.top();
                stack_hist.push(stack);
            }
        }

        switch (opcode) {
        case Pyc::BINARY_OP_A:
            {
                ASTBinary::BinOp op = ASTBinary::from_binary_op(operand);
                if (op == ASTBinary::BIN_INVALID)
                    fprintf(stderr, "Unsupported `BINARY_OP` operand value: %d\n", operand);
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                stack.push(new ASTBinary(left, right, op));
            }
            break;
        case Pyc::BINARY_ADD:
        case Pyc::BINARY_AND:
        case Pyc::BINARY_DIVIDE:
        case Pyc::BINARY_FLOOR_DIVIDE:
        case Pyc::BINARY_LSHIFT:
        case Pyc::BINARY_MODULO:
        case Pyc::BINARY_MULTIPLY:
        case Pyc::BINARY_OR:
        case Pyc::BINARY_POWER:
        case Pyc::BINARY_RSHIFT:
        case Pyc::BINARY_SUBTRACT:
        case Pyc::BINARY_TRUE_DIVIDE:
        case Pyc::BINARY_XOR:
        case Pyc::BINARY_MATRIX_MULTIPLY:
        case Pyc::INPLACE_ADD:
        case Pyc::INPLACE_AND:
        case Pyc::INPLACE_DIVIDE:
        case Pyc::INPLACE_FLOOR_DIVIDE:
        case Pyc::INPLACE_LSHIFT:
        case Pyc::INPLACE_MODULO:
        case Pyc::INPLACE_MULTIPLY:
        case Pyc::INPLACE_OR:
        case Pyc::INPLACE_POWER:
        case Pyc::INPLACE_RSHIFT:
        case Pyc::INPLACE_SUBTRACT:
        case Pyc::INPLACE_TRUE_DIVIDE:
        case Pyc::INPLACE_XOR:
        case Pyc::INPLACE_MATRIX_MULTIPLY:
            {
                ASTBinary::BinOp op = ASTBinary::from_opcode(opcode);
                if (op == ASTBinary::BIN_INVALID)
                    throw std::runtime_error("Unhandled opcode from ASTBinary::from_opcode");
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                stack.push(new ASTBinary(left, right, op));
            }
            break;
        case Pyc::BINARY_SUBSCR:
            {
                PycRef<ASTNode> subscr = stack.top();
                stack.pop();
                PycRef<ASTNode> src = stack.top();
                stack.pop();
                stack.push(new ASTSubscr(src, subscr));
            }
            break;
        case Pyc::BREAK_LOOP:
            curblock->append(new ASTKeyword(ASTKeyword::KW_BREAK));
            break;
        case Pyc::BUILD_CLASS:
            {
                PycRef<ASTNode> class_code = stack.top();
                stack.pop();
                PycRef<ASTNode> bases = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                stack.push(new ASTClass(class_code, bases, name));
            }
            break;
        case Pyc::BUILD_FUNCTION:
            {
                PycRef<ASTNode> fun_code = stack.top();
                stack.pop();
                stack.push(new ASTFunction(fun_code, {}, {}));
            }
            break;
        case Pyc::BUILD_LIST_A:
            {
                ASTList::value_t values;
                for (int i=0; i<operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTList(values));
            }
            break;
        case Pyc::BUILD_SET_A:
            {
                ASTSet::value_t values;
                for (int i=0; i<operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTSet(values));
            }
            break;
        case Pyc::BUILD_MAP_A:
            if (mod->verCompare(3, 5) >= 0) {
                auto map = new ASTMap;
                for (int i=0; i<operand; ++i) {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    map->add(key, value);
                }
                stack.push(map);
            } else {
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                stack.push(new ASTMap());
            }
            break;
        case Pyc::BUILD_CONST_KEY_MAP_A:
            // Top of stack will be a tuple of keys.
            // Values will start at TOS - 1.
            {
                PycRef<ASTNode> keys = stack.top();
                stack.pop();

                ASTConstMap::values_t values;
                values.reserve(operand);
                for (int i = 0; i < operand; ++i) {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    values.push_back(value);
                }

                stack.push(new ASTConstMap(keys, values));
            }
            break;
        case Pyc::STORE_MAP:
            {
                PycRef<ASTNode> key = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                PycRef<ASTMap> map = stack.top().cast<ASTMap>();
                map->add(key, value);
            }
            break;
        case Pyc::BUILD_SLICE_A:
            {
                if (operand == 2) {
                    PycRef<ASTNode> end = stack.top();
                    stack.pop();
                    PycRef<ASTNode> start = stack.top();
                    stack.pop();

                    if (start.type() == ASTNode::NODE_OBJECT
                            && start.cast<ASTObject>()->object() == Pyc_None) {
                        start = NULL;
                    }

                    if (end.type() == ASTNode::NODE_OBJECT
                            && end.cast<ASTObject>()->object() == Pyc_None) {
                        end = NULL;
                    }

                    if (start == NULL && end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE0));
                    } else if (start == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE2, start, end));
                    } else if (end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, start, end));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, start, end));
                    }
                } else if (operand == 3) {
                    PycRef<ASTNode> step = stack.top();
                    stack.pop();
                    PycRef<ASTNode> end = stack.top();
                    stack.pop();
                    PycRef<ASTNode> start = stack.top();
                    stack.pop();

                    if (start.type() == ASTNode::NODE_OBJECT
                            && start.cast<ASTObject>()->object() == Pyc_None) {
                        start = NULL;
                    }

                    if (end.type() == ASTNode::NODE_OBJECT
                            && end.cast<ASTObject>()->object() == Pyc_None) {
                        end = NULL;
                    }

                    if (step.type() == ASTNode::NODE_OBJECT
                            && step.cast<ASTObject>()->object() == Pyc_None) {
                        step = NULL;
                    }

                    /* We have to do this as a slice where one side is another slice */
                    /* [[a:b]:c] */

                    if (start == NULL && end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE0));
                    } else if (start == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE2, start, end));
                    } else if (end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, start, end));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, start, end));
                    }

                    PycRef<ASTNode> lhs = stack.top();
                    stack.pop();

                    if (step == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, lhs, step));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, lhs, step));
                    }
                }
            }
            break;
        case Pyc::BUILD_STRING_A:
            {
                // Nearly identical logic to BUILD_LIST
                ASTList::value_t values;
                for (int i = 0; i < operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTJoinedStr(values));
            }
            break;
        case Pyc::BUILD_TUPLE_A:
            {
                // if class is a closure code, ignore this tuple
                PycRef<ASTNode> tos = stack.top();
                if (tos && tos->type() == ASTNode::NODE_LOADBUILDCLASS) {
                    break;
                }

                ASTTuple::value_t values;
                values.resize(operand);
                for (int i=0; i<operand; i++) {
                    values[operand-i-1] = stack.top();
                    stack.pop();
                }
                stack.push(new ASTTuple(values));
            }
            break;
        case Pyc::KW_NAMES_A:
            {

                int kwparams = code->getConst(operand).cast<PycTuple>()->size();
                ASTKwNamesMap kwparamList;
                std::vector<PycRef<PycObject>> keys = code->getConst(operand).cast<PycSimpleSequence>()->values();
                for (int i = 0; i < kwparams; i++) {
                    kwparamList.add(new ASTObject(keys[kwparams - i - 1]), stack.top());
                    stack.pop();
                }
                stack.push(new ASTKwNamesMap(kwparamList));
            }
            break;
        case Pyc::CALL_A:
        case Pyc::CALL_FUNCTION_A:
        case Pyc::INSTRUMENTED_CALL_A:
            {
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;

                /* Test for the load build class function */
                stack_hist.push(stack);
                int basecnt = 0;
                ASTTuple::value_t bases;
                bases.resize(basecnt);
                PycRef<ASTNode> TOS = stack.top();
                int TOS_type = TOS.type();
                // bases are NODE_NAME and NODE_BINARY at TOS
                while (TOS_type == ASTNode::NODE_NAME || TOS_type == ASTNode::NODE_BINARY) {
                    bases.resize(basecnt + 1);
                    bases[basecnt] = TOS;
                    basecnt++;
                    stack.pop();
                    TOS = stack.top();
                    TOS_type = TOS.type();
                }
                // qualified name is PycString at TOS
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                PycRef<ASTNode> function = stack.top();
                stack.pop();
                PycRef<ASTNode> loadbuild = stack.top();
                stack.pop();
                int loadbuild_type = loadbuild.type();
                if (loadbuild_type == ASTNode::NODE_LOADBUILDCLASS) {
                    PycRef<ASTNode> call = new ASTCall(function, pparamList, kwparamList);
                    stack.push(new ASTClass(call, new ASTTuple(bases), name));
                    stack_hist.pop();
                    break;
                }
                else
                {
                    stack = stack_hist.top();
                    stack_hist.pop();
                }

                /*
                KW_NAMES(i)
                    Stores a reference to co_consts[consti] into an internal variable for use by CALL.
                    co_consts[consti] must be a tuple of strings.
                    New in version 3.11.
                */
                if (mod->verCompare(3, 11) >= 0) {
                    PycRef<ASTNode> object_or_map = stack.top();
                    if (object_or_map.type() == ASTNode::NODE_KW_NAMES_MAP) {
                        stack.pop();
                        PycRef<ASTKwNamesMap> kwparams_map = object_or_map.cast<ASTKwNamesMap>();
                        for (ASTKwNamesMap::map_t::const_iterator it = kwparams_map->values().begin(); it != kwparams_map->values().end(); it++) {
                            kwparamList.push_front(std::make_pair(it->first, it->second));
                            pparams -= 1;
                        }
                    }
                }
                else {
                    for (int i = 0; i < kwparams; i++) {
                        PycRef<ASTNode> val = stack.top();
                        stack.pop();
                        PycRef<ASTNode> key = stack.top();
                        stack.pop();
                        kwparamList.push_front(std::make_pair(key, val));
                    }
                }
                for (int i=0; i<pparams; i++) {
                    PycRef<ASTNode> param = stack.top();
                    stack.pop();
                    if (param.type() == ASTNode::NODE_FUNCTION) {
                        PycRef<ASTNode> fun_code = param.cast<ASTFunction>()->code();
                        PycRef<PycCode> code_src = fun_code.cast<ASTObject>()->object().cast<PycCode>();
                        PycRef<PycString> function_name = code_src->name();
                        if (function_name->isEqual("<lambda>")) {
                            pparamList.push_front(param);
                        } else {
                            // Decorator used
                            PycRef<ASTNode> decor_name = new ASTName(function_name);
                            curblock->append(new ASTStore(param, decor_name));

                            pparamList.push_front(decor_name);
                        }
                    } else {
                        pparamList.push_front(param);
                    }
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                if ((opcode == Pyc::CALL_A || opcode == Pyc::INSTRUMENTED_CALL_A) &&
                        stack.top() == nullptr) {
                    stack.pop();
                }

                stack.push(new ASTCall(func, pparamList, kwparamList));
            }
            break;
        case Pyc::CALL_FUNCTION_VAR_A:
            {
                PycRef<ASTNode> var = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setVar(var);
                stack.push(call);
            }
            break;
        case Pyc::CALL_FUNCTION_KW_A:
            {
                PycRef<ASTNode> kw = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setKW(kw);
                stack.push(call);
            }
            break;
        case Pyc::CALL_FUNCTION_VAR_KW_A:
            {
                PycRef<ASTNode> kw = stack.top();
                stack.pop();
                PycRef<ASTNode> var = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setKW(kw);
                call.cast<ASTCall>()->setVar(var);
                stack.push(call);
            }
            break;
        case Pyc::CALL_METHOD_A:
            {
                ASTCall::pparam_t pparamList;
                for (int i = 0; i < operand; i++) {
                    PycRef<ASTNode> param = stack.top();
                    stack.pop();
                    if (param.type() == ASTNode::NODE_FUNCTION) {
                        PycRef<ASTNode> fun_code = param.cast<ASTFunction>()->code();
                        PycRef<PycCode> code_src = fun_code.cast<ASTObject>()->object().cast<PycCode>();
                        PycRef<PycString> function_name = code_src->name();
                        if (function_name->isEqual("<lambda>")) {
                            pparamList.push_front(param);
                        } else {
                            // Decorator used
                            PycRef<ASTNode> decor_name = new ASTName(function_name);
                            curblock->append(new ASTStore(param, decor_name));

                            pparamList.push_front(decor_name);
                        }
                    } else {
                        pparamList.push_front(param);
                    }
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                stack.push(new ASTCall(func, pparamList, ASTCall::kwparam_t()));
            }
            break;
        case Pyc::CONTINUE_LOOP_A:
            curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
            break;
        case Pyc::COMPARE_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                auto arg = operand;
                if (mod->verCompare(3, 12) == 0)
                    arg >>= 4; // changed under GH-100923
                else if (mod->verCompare(3, 13) >= 0)
                    arg >>= 5;
                stack.push(new ASTCompare(left, right, arg));
            }
            break;
        case Pyc::CONTAINS_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                // The operand will be 0 for 'in' and 1 for 'not in'.
                stack.push(new ASTCompare(left, right, operand ? ASTCompare::CMP_NOT_IN : ASTCompare::CMP_IN));
            }
            break;
        case Pyc::DELETE_ATTR_A:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                curblock->append(new ASTDelete(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR)));
            }
            break;
        case Pyc::DELETE_GLOBAL_A:
            code->markGlobal(code->getName(operand));
            /* Fall through */
        case Pyc::DELETE_NAME_A:
            {
                PycRef<PycString> varname = code->getName(operand);

                if (varname->length() >= 2 && varname->value()[0] == '_'
                        && varname->value()[1] == '[') {
                    /* Don't show deletes that are a result of list comps. */
                    break;
                }

                PycRef<ASTNode> name = new ASTName(varname);
                curblock->append(new ASTDelete(name));
            }
            break;
        case Pyc::DELETE_FAST_A:
            {
                PycRef<ASTNode> name;

                if (mod->verCompare(1, 3) < 0)
                    name = new ASTName(code->getName(operand));
                else
                    name = new ASTName(code->getLocal(operand));

                if (name.cast<ASTName>()->name()->value()[0] == '_'
                        && name.cast<ASTName>()->name()->value()[1] == '[') {
                    /* Don't show deletes that are a result of list comps. */
                    break;
                }

                /* Python 3.11+: suppress the implicit `del <name>` that clears an
                   `except ... as <name>` alias at the end of its clause. */
                if (zce_active_handler >= 0 && zce_as_name != NULL
                        && name.cast<ASTName>()->name()->isEqual(zce_as_name->strValue())) {
                    break;
                }

                curblock->append(new ASTDelete(name));
            }
            break;
        case Pyc::DELETE_SLICE_0:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE0))));
            }
            break;
        case Pyc::DELETE_SLICE_1:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE1, upper))));
            }
            break;
        case Pyc::DELETE_SLICE_2:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE2, NULL, lower))));
            }
            break;
        case Pyc::DELETE_SLICE_3:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE3, upper, lower))));
            }
            break;
        case Pyc::DELETE_SUBSCR:
            {
                PycRef<ASTNode> key = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, key)));
            }
            break;
        case Pyc::DUP_TOP:
            {
                if (stack.top().type() == PycObject::TYPE_NULL) {
                    stack.push(stack.top());
                } else if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    auto chainstore = stack.top();
                    stack.pop();
                    stack.push(stack.top());
                    stack.push(chainstore);
                } else {
                    stack.push(stack.top());
                    ASTNodeList::list_t targets;
                    stack.push(new ASTChainStore(targets, stack.top()));
                }
            }
            break;
        case Pyc::DUP_TOP_TWO:
            {
                PycRef<ASTNode> first = stack.top();
                stack.pop();
                PycRef<ASTNode> second = stack.top();

                stack.push(first);
                stack.push(second);
                stack.push(first);
            }
            break;
        case Pyc::DUP_TOPX_A:
            {
                std::stack<PycRef<ASTNode> > first;
                std::stack<PycRef<ASTNode> > second;

                for (int i = 0; i < operand; i++) {
                    PycRef<ASTNode> node = stack.top();
                    stack.pop();
                    first.push(node);
                    second.push(node);
                }

                while (first.size()) {
                    stack.push(first.top());
                    first.pop();
                }

                while (second.size()) {
                    stack.push(second.top());
                    second.pop();
                }
            }
            break;
        case Pyc::END_FINALLY:
            {
                bool isFinally = false;
                if (curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    PycRef<ASTBlock> final = curblock;
                    blocks.pop();

                    stack = stack_hist.top();
                    stack_hist.pop();

                    curblock = blocks.top();
                    curblock->append(final.cast<ASTNode>());
                    isFinally = true;
                } else if (curblock->blktype() == ASTBlock::BLK_EXCEPT) {
                    blocks.pop();
                    PycRef<ASTBlock> prev = curblock;

                    bool isUninitAsyncFor = false;
                    if (blocks.top()->blktype() == ASTBlock::BLK_CONTAINER) {
                        auto container = blocks.top();
                        blocks.pop();
                        auto asyncForBlock = blocks.top();
                        isUninitAsyncFor = asyncForBlock->blktype() == ASTBlock::BLK_ASYNCFOR && !asyncForBlock->inited();
                        if (isUninitAsyncFor) {
                            auto tryBlock = container->nodes().front().cast<ASTBlock>();
                            if (!tryBlock->nodes().empty() && tryBlock->blktype() == ASTBlock::BLK_TRY) {
                                auto store = tryBlock->nodes().front().try_cast<ASTStore>();
                                if (store) {
                                    asyncForBlock.cast<ASTIterBlock>()->setIndex(store->dest());
                                }
                            }
                            curblock = blocks.top();
                            stack = stack_hist.top();
                            stack_hist.pop();
                            if (!curblock->inited())
                                fprintf(stderr, "Error when decompiling 'async for'.\n");
                        } else {
                            blocks.push(container);
                        }
                    }

                    if (!isUninitAsyncFor) {
                        if (curblock->size() != 0) {
                            blocks.top()->append(curblock.cast<ASTNode>());
                        }

                        curblock = blocks.top();

                        /* Turn it into an else statement. */
                        if (curblock->end() != pos || curblock.cast<ASTContainerBlock>()->hasFinally()) {
                            PycRef<ASTBlock> elseblk = new ASTBlock(ASTBlock::BLK_ELSE, prev->end());
                            elseblk->init();
                            blocks.push(elseblk);
                            curblock = blocks.top();
                        }
                        else {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                    }
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    /* This marks the end of the except block(s). */
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (!cont->hasFinally() || isFinally) {
                        /* If there's no finally block, pop the container. */
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(cont.cast<ASTNode>());
                    }
                }
            }
            break;
        case Pyc::EXEC_STMT:
            {
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> loc = stack.top();
                stack.pop();
                PycRef<ASTNode> glob = stack.top();
                stack.pop();
                PycRef<ASTNode> stmt = stack.top();
                stack.pop();

                curblock->append(new ASTExec(stmt, glob, loc));
            }
            break;
        case Pyc::FOR_ITER_A:
        case Pyc::INSTRUMENTED_FOR_ITER_A:
            {
                PycRef<ASTNode> iter = stack.top(); // Iterable
                stack.pop();
                /* Pop it? Don't pop it? */

                int end;
                bool comprehension = false;

                // before 3.8, there is a SETUP_LOOP instruction with block start and end position,
                //    the operand is usually a jump to a POP_BLOCK instruction
                // after 3.8, block extent has to be inferred implicitly; the operand is a jump to a position after the for block
                if (mod->majorVer() == 3 && mod->minorVer() >= 8) {
                    end = operand;
                    if (mod->verCompare(3, 10) >= 0)
                        end *= sizeof(uint16_t); // // BPO-27129
                    end += pos;
                    comprehension = strcmp(code->name()->value(), "<listcomp>") == 0;
                } else {
                    PycRef<ASTBlock> top = blocks.top();
                    end = top->end(); // block end position from SETUP_LOOP
                    if (top->blktype() == ASTBlock::BLK_WHILE) {
                        blocks.pop();
                    } else {
                        comprehension = true;
                    }
                }

                PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_FOR, curpos, end, iter);
                forblk->setComprehension(comprehension);
                blocks.push(forblk.cast<ASTBlock>());
                curblock = blocks.top();

                stack.push(NULL);
            }
            break;
        case Pyc::FOR_LOOP_A:
            {
                PycRef<ASTNode> curidx = stack.top(); // Current index
                stack.pop();
                PycRef<ASTNode> iter = stack.top(); // Iterable
                stack.pop();

                bool comprehension = false;
                PycRef<ASTBlock> top = blocks.top();
                if (top->blktype() == ASTBlock::BLK_WHILE) {
                    blocks.pop();
                } else {
                    comprehension = true;
                }
                PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_FOR, curpos, top->end(), iter);
                forblk->setComprehension(comprehension);
                blocks.push(forblk.cast<ASTBlock>());
                curblock = blocks.top();

                /* Python Docs say:
                      "push the sequence, the incremented counter,
                       and the current item onto the stack." */
                stack.push(iter);
                stack.push(curidx);
                stack.push(NULL); // We can totally hack this >_>
            }
            break;
        case Pyc::RERAISE:
        case Pyc::RERAISE_A:
        case Pyc::RAISE_EXCEPTION:
            /* Python 3.11+: within a zero-cost handler the trailing RERAISE marks
               the point where no user except clause matched.  When we see it at
               container level, the handler is complete: pop the container off the
               block stack and deactivate handler reconstruction. */
            if (zce_active_handler >= 0
                    && curblock->blktype() == ASTBlock::BLK_CONTAINER
                    && curblock == zce_active_container.cast<ASTBlock>()) {
                /* In 3.11+ the compiler duplicates the code that follows the
                   try/except (the "fall through" continuation) into the
                   handler's exit path, right after POP_EXCEPT.  Because we walk
                   the bytecode linearly, those duplicated statements leak into
                   the container as non-block siblings of the try/except/finally
                   sub-blocks.  A container only ever holds structural
                   sub-blocks, so any trailing non-block node is such a leaked
                   duplicate — drop it. */
                while (curblock->size()
                        && curblock->nodes().back().type() != ASTNode::NODE_BLOCK) {
                    curblock->removeLast();
                }
                blocks.pop();
                curblock = blocks.top();
                if (!stack_hist.empty()) {
                    stack = stack_hist.top();
                    stack_hist.pop();
                }
                if (!zce_handler_stack.empty()) {
                    ZceHandlerState parent = zce_handler_stack.back();
                    zce_handler_stack.pop_back();
                    zce_active_handler = parent.handler;
                    zce_active_container = parent.container;
                    zce_expect_as = parent.expect_as;
                    zce_expect_check = parent.expect_check;
                    zce_as_name = parent.as_name;
                } else {
                    zce_active_handler = -1;
                    zce_active_container = nullptr;
                    zce_expect_as = false;
                    zce_expect_check = false;
                    zce_as_name = nullptr;
                }
            }
            break;
        case Pyc::GET_AITER:
            {
                // Logic similar to FOR_ITER_A
                PycRef<ASTNode> iter = stack.top(); // Iterable
                stack.pop();

                PycRef<ASTBlock> top = blocks.top();
                if (top->blktype() == ASTBlock::BLK_WHILE) {
                    blocks.pop();
                    PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_ASYNCFOR, curpos, top->end(), iter);
                    blocks.push(forblk.cast<ASTBlock>());
                    curblock = blocks.top();
                    stack.push(nullptr);
                } else {
                     fprintf(stderr, "Unsupported use of GET_AITER outside of SETUP_LOOP\n");
                }
            }
            break;
        case Pyc::GET_ANEXT:
            break;
        case Pyc::FORMAT_VALUE_A:
            {
                auto conversion_flag = static_cast<ASTFormattedValue::ConversionFlag>(operand);
                PycRef<ASTNode> format_spec = nullptr;
                if (conversion_flag & ASTFormattedValue::HAVE_FMT_SPEC) {
                    format_spec = stack.top();
                    stack.pop();
                }
                auto val = stack.top();
                stack.pop();
                stack.push(new ASTFormattedValue(val, conversion_flag, format_spec));
            }
            break;
        case Pyc::GET_AWAITABLE:
            {
                PycRef<ASTNode> object = stack.top();
                stack.pop();
                stack.push(new ASTAwaitable(object));
            }
            break;
        case Pyc::GET_ITER:
        case Pyc::GET_YIELD_FROM_ITER:
            /* We just entirely ignore this */
            break;
        case Pyc::IMPORT_NAME_A:
            if (mod->majorVer() == 1) {
                stack.push(new ASTImport(new ASTName(code->getName(operand)), NULL));
            } else {
                PycRef<ASTNode> fromlist = stack.top();
                stack.pop();
                if (mod->verCompare(2, 5) >= 0)
                    stack.pop();    // Level -- we don't care
                stack.push(new ASTImport(new ASTName(code->getName(operand)), fromlist));
            }
            break;
        case Pyc::CALL_KW_METHOD_A:
        case Pyc::CALL_NO_KW_A:
        case Pyc::CALL_NO_KW_METHOD_A: {
            // For these, pop positional args, then function/method
            ASTCall::pparam_t pparamList;
            for (int i = 0; i < operand; ++i) {
                pparamList.push_front(stack.top());
                stack.pop();
            }
            PycRef<ASTNode> func = stack.top();
            stack.pop();
            stack.push(new ASTCall(func, pparamList, ASTCall::kwparam_t()));
            break;
        }
        case Pyc::CALL_FINALLY_A:
            // Used for exception/finally setup/teardown; no AST action needed.
            break;
        case Pyc::CALL_KW_A: {
            // Top of stack is tuple of keyword names
            PycRef<ASTNode> kw_names = stack.top();
            stack.pop();

            int kwparams = kw_names.cast<ASTObject>()->object().cast<PycTuple>()->size();
            ASTCall::kwparam_t kwparamList;
            std::vector<PycRef<PycObject>> keys = kw_names.cast<ASTObject>()->object().cast<PycTuple>()->values();
            for (int i = 0; i < kwparams; i++) {
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                kwparamList.push_front(std::make_pair(new ASTObject(keys[kwparams - i - 1]), value));
            }

            // Now pop positional parameters
            ASTCall::pparam_t pparamList;
            for (int i = 0; i < (operand - kwparams); i++) {
                pparamList.push_front(stack.top());
                stack.pop();
            }

            PycRef<ASTNode> func = stack.top();
            stack.pop();

            stack.push(new ASTCall(func, pparamList, kwparamList));
            break;
}
        case Pyc::CALL_INTRINSIC_1_A:
        case Pyc::CALL_INTRINSIC_2_A: {
            int num_args = (opcode == Pyc::CALL_INTRINSIC_1_A) ? 1 : 2;
            ASTCall::pparam_t pparams;
            for (int i = 0; i < num_args; ++i) {
                pparams.push_front(stack.top());
                stack.pop();
            }
            // Instead of constructing a new ASTName with a string, just use a placeholder:
            PycRef<ASTNode> intrinsic_func = nullptr; // or stack.top(), or skip
            stack.push(new ASTCall(intrinsic_func, pparams, ASTCall::kwparam_t()));
            break;
        }

        case Pyc::INSTRUMENTED_CALL_FUNCTION_EX_A:
        case Pyc::CALL_FUNCTION_EX_A: {
            PycRef<ASTNode> kwargs;
            if (operand & 0x01) {
                kwargs = stack.top();
                stack.pop();
            }
            PycRef<ASTNode> args = stack.top();
            stack.pop();
            PycRef<ASTNode> func = stack.top();
            stack.pop();

            ASTCall::pparam_t pparams;
            ASTCall::kwparam_t kwparams;

            // No ASTStarred/ASTDoubleStarred available; just add args as-is
            if (args)
                pparams.push_back(args); // TODO: mark as unpacked
            if (kwargs)
                kwparams.push_back(std::make_pair(kwargs, nullptr)); // TODO: mark as double-starred

            stack.push(new ASTCall(func, pparams, kwparams));
            break;
}
        case Pyc::IMPORT_FROM_A:
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::IMPORT_STAR:
            {
                PycRef<ASTNode> import = stack.top();
                stack.pop();
                curblock->append(new ASTStore(import, NULL));
            }
            break;
        case Pyc::BUILD_LIST_UNPACK_A:
        case Pyc::BUILD_SET_UNPACK_A:
        case Pyc::BUILD_MAP_UNPACK_A:
        case Pyc::BUILD_TUPLE_UNPACK_A:
        case Pyc::BUILD_TUPLE_UNPACK_WITH_CALL_A:
        case Pyc::BUILD_MAP_UNPACK_WITH_CALL_A:
            {
                // TODO: Implement starred/unpacking logic for these opcodes!
                // For now, just pop and push a dummy node.
                for (int i = 0; i < operand; ++i) {
                    stack.pop();
                }
                stack.push(new ASTNode(ASTNode::NODE_INVALID)); // or some placeholder
            }
            break;
        case Pyc::BINARY_CALL:
            {
                // TODO: Implement logic for BINARY_CALL
                stack.pop(); // kwargs
                stack.pop(); // args
                stack.pop(); // func
                stack.push(new ASTNode(ASTNode::NODE_INVALID));
            }
            break;
        case Pyc::IS_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                // The operand will be 0 for 'is' and 1 for 'is not'.
                stack.push(new ASTCompare(left, right, operand ? ASTCompare::CMP_IS_NOT : ASTCompare::CMP_IS));
            }
            break;

        case Pyc::ACCESS_MODE_A:
                //         Python 3.12+: Internal check for open() mode string, not user-visible.
                //         No AST action required.
                break;

        case Pyc::BEFORE_ASYNC_WITH:
        case Pyc::BEFORE_WITH:
                //         Context manager setup for (async) with-statement, handled elsewhere.
                //         No AST node or Python code emission here.
                break;
        case Pyc::COPY_DICT_WITHOUT_KEYS:
                //         Internal opcode for dictionary unpacking without certain keys (Python 3.9+).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::CHECK_EG_MATCH:
                //         Internal opcode for "except ... as ..." (exception group matching, Python 3.11+).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::PUSH_EXC_INFO:
                /* Python 3.11+: marks the very start of a handler.  Push a
                   placeholder for the current exception so CHECK_EXC_MATCH has a
                   left operand, and open the first (typed-or-bare) except block
                   inside the active container.  When there are multiple except
                   clauses each subsequent one re-enters here. */
                if (zce_active_handler >= 0) {
                    stack.push(new ASTName(new PycString));
                    zce_expect_check = true;  // Next: CHECK_EXC_MATCH or POP_TOP (bare except)
                }
                break;

        case Pyc::CHECK_EXC_MATCH:
                /* Python 3.11+: TOS is the exception type to match against the
                   active exception (pushed by PUSH_EXC_INFO).  Build a
                   CMP_EXCEPTION compare so the following POP_JUMP_IF_FALSE opens
                   a typed `except <type>:` block via the shared machinery. */
                if (zce_active_handler >= 0) {
                    PycRef<ASTNode> right = stack.top();
                    stack.pop();
                    PycRef<ASTNode> left = stack.top();
                    stack.pop();
                    stack.push(new ASTCompare(left, right, ASTCompare::CMP_EXCEPTION));
                    zce_expect_check = false;  // Consumed
                }
                break;
        case Pyc::CLEANUP_THROW:
                //         Internal opcode for cleaning up after a generator throw (Python 3.11+).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::CONVERT_VALUE_A:
                //         Internal opcode for value conversion, used for handling annotations or internal conversions.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::COPY_FREE_VARS_A:
                //         Internal opcode for copying closure variables for nested functions.
                //         Not user-visible, no AST node or Python code emission needed.
                break;
        case Pyc::DELETE_DEREF_A:
                //         Deletes a cell variable (closure variable) in the current scope.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::DICT_MERGE_A:
                //         Internal opcode for merging dictionaries during dictionary unpacking (e.g., {**a, **b}).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::DICT_UPDATE_A:
                //         Internal opcode for updating a dictionary with another (e.g., dict.update()).
                //         Not user-visible, no AST node or Python code emission needed.
                break;
        case Pyc::END_ASYNC_FOR:
                //         Marks the end of an async for loop (Python 3.6+).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::END_SEND:
                //         Internal opcode for async generator/send protocol.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::ENTER_EXECUTOR_A:
                //         Internal opcode for entering an executor context (used by async/await machinery).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::EXIT_INIT_CHECK:
                //         Internal opcode for checking initialization state on context exit.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::EXTENDED_ARG_A:
                //         Used to extend the argument of the following opcode (legacy opcode for >8-bit bytecode args).
                //         No AST node or Python code emission needed.
                break;
        case Pyc::FORMAT_SIMPLE:
                //         Internal opcode for simple string formatting using f-strings or .format(), no formatting spec.
                //         Not user-visible, formatting is handled at a higher AST level.
                break;

        case Pyc::LOAD_COMMON_CONSTANT_A:
            {
                /* Python 3.14+: push a well-known builtin by index. */
                static const char* common_constants[] = {
                    "AssertionError", "NotImplementedError", "tuple", "all", "any"
                };
                static const size_t common_constants_len =
                    sizeof(common_constants) / sizeof(common_constants[0]);
                PycRef<PycString> name = new PycString;
                name->setValue((static_cast<size_t>(operand) < common_constants_len)
                               ? common_constants[operand] : "<COMMON_CONSTANT>");
                stack.push(new ASTName(name));
            }
            break;

        case Pyc::LOAD_SPECIAL_A:
            {
                /* Python 3.14+: replace TOS object with a bound special method
                   (used by the with/async-with protocol). We model it as an
                   attribute access; the surrounding with-block handling ignores
                   the exact form. */
                static const char* special_methods[] = {
                    "__enter__", "__exit__", "__aenter__", "__aexit__"
                };
                static const size_t special_methods_len =
                    sizeof(special_methods) / sizeof(special_methods[0]);
                PycRef<PycString> meth = new PycString;
                meth->setValue((static_cast<size_t>(operand) < special_methods_len)
                               ? special_methods[operand] : "<SPECIAL>");
                PycRef<ASTNode> obj = stack.top();
                stack.pop();
                stack.push(new ASTBinary(obj, new ASTName(meth), ASTBinary::BIN_ATTR));
            }
            break;

        case Pyc::STORE_FAST_MAYBE_NULL_A:
                //         Python 3.14+: internal store used by the with/except* machinery.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::ANNOTATIONS_PLACEHOLDER:
        case Pyc::RESERVED:
                //         Python 3.14+ placeholder/reserved slots with no runtime effect.
                break;

        case Pyc::FORMAT_WITH_SPEC:
                //         Internal opcode for string formatting with a format specifier (e.g., f"{x:.2f}").
                //         Not user-visible, formatting is handled at a higher AST level.
                break;

        case Pyc::LOAD_GLOBALS:
                //         Loads the globals dictionary for the current module or function.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::LOAD_LOCAL_A:
                //         Loads a local variable by index (Python 3.12+).
                //         Not user-visible, handled by variable access in AST.
                break;
        case Pyc::GET_AWAITABLE_A:
                //         Internal opcode for getting an awaitable object (async/await handling).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::GET_LEN:
                //         Internal opcode for retrieving the length of an object (used in len() and for-loops).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::INSTRUMENTED_CALL_KW_A:
                //         Internal opcode for instrumented function calls with keyword arguments (used for profiling/tracing).
                //         Not user-visible, no AST node or Python code emission needed.
                break;
        case Pyc::INSTRUMENTED_END_FOR_A:
                //         Internal opcode for ending an instrumented 'for' loop (used for coverage or profiling).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::INSTRUMENTED_END_SEND_A:
                //         Internal opcode for ending an instrumented send operation (used for async generators, profiling).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::INSTRUMENTED_INSTRUCTION_A:
                //         Internal opcode for an instrumented instruction (used by profiling or tracing tools).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::INSTRUMENTED_LINE_A:
                //         Internal opcode marking an instrumented line (used for coverage or tracing).
                //         Not user-visible, no AST node or Python code emission needed.
                break;
        case Pyc::INSTRUMENTED_LOAD_SUPER_ATTR_A:
                //         Internal opcode for instrumented loading of a super() attribute (used for profiling/tracing).
                //         Not user-visible, no AST node or Python code emission needed.
                break;
        case Pyc::LIST_TO_TUPLE:
                //         Internal opcode to convert a list to a tuple (e.g., for unpacking or star expressions).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::LOAD_ASSERTION_ERROR:
                //         Loads the built-in AssertionError for assert statements.
                //         Not user-visible, handled at a higher AST level.
                break;

        case Pyc::LOAD_FAST_AND_CLEAR_A:
                //         Loads a local variable and clears it (Python 3.11+).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::LOAD_FROM_DICT_OR_DEREF_A:
                //         Loads a value from a dictionary or a closure cell, as needed.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::LOAD_FROM_DICT_OR_GLOBALS_A:
                //         Loads a value from a dictionary or globals, as needed.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::LOAD_SUPER_ATTR_A:
                //         Loads an attribute from super() (Python 3.11+), used for method resolution order.
                //         Not user-visible, no AST node or Python code emission needed.
                break;
        case Pyc::MAKE_CELL_A:
                //         Creates a new cell variable for closures (Python 3.11+).
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::MAP_ADD_A:
                //         Adds a key-value pair to a map/dictionary during dictionary comprehensions or builds.
                //         Not user-visible, no AST node or Python code emission needed.
                break;

        case Pyc::MATCH_CLASS_A:
                //         Internal opcode for matching a class pattern in a match/case statement (Python 3.10+).
                //         Not user-visible, pattern matching handled at a higher AST level.
                break;

        case Pyc::MATCH_KEYS:
                //         Internal opcode for matching dictionary keys in pattern matching (Python 3.10+).
                //         Not user-visible, pattern matching handled at a higher AST level.
                break;

        case Pyc::MATCH_MAPPING:
                //         Internal opcode for matching mapping (dictionary) patterns in match/case (Python 3.10+).
                //         Not user-visible, pattern matching handled at a higher AST level.
                break;

        case Pyc::MATCH_SEQUENCE:
                //         Internal opcode for matching sequence patterns in match/case (Python 3.10+).
                //         Not user-visible, pattern matching handled at a higher AST level.
                break;
        case Pyc::INSTRUMENTED_POP_JUMP_IF_FALSE_A:
        case Pyc::INSTRUMENTED_POP_JUMP_IF_NONE_A:
        case Pyc::INSTRUMENTED_POP_JUMP_IF_NOT_NONE_A:
        case Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A:
        case Pyc::INTERPRETER_EXIT:
        case Pyc::JUMP_IF_FALSE_A:
        case Pyc::JUMP_IF_FALSE_OR_POP_A:
        case Pyc::JUMP_IF_NOT_EXC_MATCH_A:
        case Pyc::JUMP_IF_TRUE_A:
        case Pyc::JUMP_IF_TRUE_OR_POP_A:
        case Pyc::POP_JUMP_BACKWARD_IF_TRUE_A:
        case Pyc::POP_JUMP_FORWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_FORWARD_IF_NONE_A:
        case Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A:
        case Pyc::POP_JUMP_FORWARD_IF_TRUE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_NONE_A:
        case Pyc::POP_FINALLY_A:
        case Pyc::PRINT_EXPR:
        case Pyc::POP_JUMP_IF_FALSE_A:
        case Pyc::POP_JUMP_IF_NONE_A:
        case Pyc::POP_JUMP_IF_NOT_NONE_A:
        case Pyc::POP_JUMP_IF_TRUE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_NOT_NONE_A:
        case Pyc::PREP_RERAISE_STAR:
        case Pyc::ASYNC_GEN_WRAP:
        case Pyc::BEGIN_FINALLY:
        case Pyc::ROT_N_A:
        case Pyc::RESERVE_FAST_A:
        case Pyc::RETURN_GENERATOR:
        case Pyc::SEND_A:
        case Pyc::SET_ADD:
        case Pyc::SET_ADD_A:
        case Pyc::SETUP_ASYNC_WITH_A:
        case Pyc::SET_FUNC_ARGS_A:
        case Pyc::STOP_CODE:
        case Pyc::STORE_ANNOTATION_A:
        case Pyc::SET_FUNCTION_ATTRIBUTE_A:
        case Pyc::STORE_FAST_LOAD_FAST_A:
        case Pyc::STORE_FAST_STORE_FAST_A:
        case Pyc::TO_BOOL:
        case Pyc::UNPACK_EX_A:
        case Pyc::UNPACK_VARARG_A:
        case Pyc::UNPACK_ARG_A:
        case Pyc::YIELD_VALUE_A:

            {
                PycRef<ASTNode> cond = stack.top();
                PycRef<ASTCondBlock> ifblk;
                int popped = ASTCondBlock::UNINITED;

                if (opcode == Pyc::POP_JUMP_IF_FALSE_A
                        || opcode == Pyc::POP_JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_FALSE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A) {
                    /* Pop condition before the jump */
                    stack.pop();
                    popped = ASTCondBlock::PRE_POPPED;
                }

                /* Store the current stack for the else statement(s) */
                stack_hist.push(stack);

                if (opcode == Pyc::JUMP_IF_FALSE_OR_POP_A
                        || opcode == Pyc::JUMP_IF_TRUE_OR_POP_A) {
                    /* Pop condition only if condition is met */
                    stack.pop();
                    popped = ASTCondBlock::POPPED;
                }

                /* "Jump if true" means "Jump if not false" */
                bool neg = opcode == Pyc::JUMP_IF_TRUE_A
                        || opcode == Pyc::JUMP_IF_TRUE_OR_POP_A
                        || opcode == Pyc::POP_JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A;

                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // // BPO-27129
                if (mod->verCompare(3, 12) >= 0
                        || opcode == Pyc::JUMP_IF_FALSE_A
                        || opcode == Pyc::JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A) {
                    /* Offset is relative in these cases */
                    offs += pos;
                }

                if (cond.type() == ASTNode::NODE_COMPARE
                        && cond.cast<ASTCompare>()->op() == ASTCompare::CMP_EXCEPTION) {
                    if (curblock->blktype() == ASTBlock::BLK_EXCEPT
                            && curblock.cast<ASTCondBlock>()->cond() == NULL) {
                        blocks.pop();
                        curblock = blocks.top();

                        stack_hist.pop();
                    }

                    ifblk = new ASTCondBlock(ASTBlock::BLK_EXCEPT, offs, cond.cast<ASTCompare>()->right(), false);
                } else if (curblock->blktype() == ASTBlock::BLK_ELSE
                           && curblock->size() == 0) {
                    /* Collapse into elif statement */
                    blocks.pop();
                    stack = stack_hist.top();
                    stack_hist.pop();
                    ifblk = new ASTCondBlock(ASTBlock::BLK_ELIF, offs, cond, neg);
                } else if (curblock->size() == 0 && !curblock->inited()
                           && curblock->blktype() == ASTBlock::BLK_WHILE) {
                    /* The condition for a while loop */
                    PycRef<ASTBlock> top = blocks.top();
                    blocks.pop();
                    ifblk = new ASTCondBlock(top->blktype(), offs, cond, neg);

                    /* We don't store the stack for loops! Pop it! */
                    stack_hist.pop();
                } else if (curblock->size() == 0 && curblock->end() <= offs
                           && (curblock->blktype() == ASTBlock::BLK_IF
                           || curblock->blktype() == ASTBlock::BLK_ELIF
                           || curblock->blktype() == ASTBlock::BLK_WHILE)) {
                    PycRef<ASTNode> newcond;
                    PycRef<ASTCondBlock> top = curblock.cast<ASTCondBlock>();
                    PycRef<ASTNode> cond1 = top->cond();
                    blocks.pop();

                    if (curblock->blktype() == ASTBlock::BLK_WHILE) {
                        stack_hist.pop();
                    } else {
                        FastStack s_top = stack_hist.top();
                        stack_hist.pop();
                        stack_hist.pop();
                        stack_hist.push(s_top);
                    }

                    if (curblock->end() == offs
                            || (curblock->end() == curpos && !top->negative())) {
                        /* if blah and blah */
                        newcond = new ASTBinary(cond1, cond, ASTBinary::BIN_LOG_AND);
                    } else {
                        /* if blah or blah */
                        newcond = new ASTBinary(cond1, cond, ASTBinary::BIN_LOG_OR);
                    }
                    ifblk = new ASTCondBlock(top->blktype(), offs, newcond, neg);
                } else if (curblock->blktype() == ASTBlock::BLK_FOR
                            && curblock.cast<ASTIterBlock>()->isComprehension()
                            && mod->verCompare(2, 7) >= 0) {
                    /* Comprehension condition */
                    curblock.cast<ASTIterBlock>()->setCondition(cond);
                    stack_hist.pop();
                    // TODO: Handle older python versions, where condition
                    // is laid out a little differently.
                    break;
                } else {
                    /* Plain old if statement */
                    ifblk = new ASTCondBlock(ASTBlock::BLK_IF, offs, cond, neg);
                }

                if (popped)
                    ifblk->init(popped);

                blocks.push(ifblk.cast<ASTBlock>());
                curblock = blocks.top();
            }
            break;
        case Pyc::JUMP_BACKWARD_A:
        case Pyc::INSTRUMENTED_JUMP_BACKWARD_A:
        case Pyc::JUMP_BACKWARD_NO_INTERRUPT_A:
        {
            /* Python 3.11+: inside a zero-cost except clause the trailing
               JUMP_BACKWARD_NO_INTERRUPT merely resumes the shared continuation
               after the try/except; it is not a loop 'continue'. */
            if (opcode == Pyc::JUMP_BACKWARD_NO_INTERRUPT_A
                    && zce_active_handler >= 0) {
                break;
            }

            int delta = operand;
            if (mod->verCompare(3, 10) >= 0) {
                delta *= sizeof(uint16_t);
            }
            int offs = pos - delta;

            if (curblock->blktype() == ASTBlock::BLK_FOR) {
                bool is_jump_to_start = offs == curblock.cast<ASTIterBlock>()->start();
                bool should_pop_for_block = curblock.cast<ASTIterBlock>()->isComprehension();
                bool should_add_for_block = mod->majorVer() == 3 && mod->minorVer() >= 8 && is_jump_to_start && !curblock.cast<ASTIterBlock>()->isComprehension();

                if (should_pop_for_block || should_add_for_block) {
                    PycRef<ASTNode> top = stack.top();

                    if (top.type() == ASTNode::NODE_COMPREHENSION) {
                        PycRef<ASTComprehension> comp = top.cast<ASTComprehension>();
                        comp->addGenerator(curblock.cast<ASTIterBlock>());
                    }

                    PycRef<ASTBlock> tmp = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    if (should_add_for_block) {
                        curblock->append(tmp.cast<ASTNode>());
                    }
                }
            } else if (curblock->blktype() == ASTBlock::BLK_ELSE) {
                stack = stack_hist.top();
                stack_hist.pop();

                blocks.pop();
                blocks.top()->append(curblock.cast<ASTNode>());
                curblock = blocks.top();

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER
                        && !curblock.cast<ASTContainerBlock>()->hasFinally()) {
                    blocks.pop();
                    blocks.top()->append(curblock.cast<ASTNode>());
                    curblock = blocks.top();
                }
            } else {
                curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
            }

            break;
        }
        case Pyc::JUMP_ABSOLUTE_A:
            {
                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // // BPO-27129

                if (offs < pos) {
                    if (curblock->blktype() == ASTBlock::BLK_FOR) {
                        bool is_jump_to_start = offs == curblock.cast<ASTIterBlock>()->start();
                        bool should_pop_for_block = curblock.cast<ASTIterBlock>()->isComprehension();
                        // in v3.8, SETUP_LOOP is deprecated and for blocks aren't terminated by POP_BLOCK, so we add them here
                        bool should_add_for_block = mod->majorVer() == 3 && mod->minorVer() >= 8 && is_jump_to_start && !curblock.cast<ASTIterBlock>()->isComprehension();

                        if (should_pop_for_block || should_add_for_block) {
                            PycRef<ASTNode> top = stack.top();

                            if (top.type() == ASTNode::NODE_COMPREHENSION) {
                                PycRef<ASTComprehension> comp = top.cast<ASTComprehension>();

                                comp->addGenerator(curblock.cast<ASTIterBlock>());
                            }

                            PycRef<ASTBlock> tmp = curblock;
                            blocks.pop();
                            curblock = blocks.top();
                            if (should_add_for_block) {
                                curblock->append(tmp.cast<ASTNode>());
                            }
                        }
                    } else if (curblock->blktype() == ASTBlock::BLK_ELSE) {
                        stack = stack_hist.top();
                        stack_hist.pop();

                        blocks.pop();
                        blocks.top()->append(curblock.cast<ASTNode>());
                        curblock = blocks.top();

                        if (curblock->blktype() == ASTBlock::BLK_CONTAINER
                                && !curblock.cast<ASTContainerBlock>()->hasFinally()) {
                            blocks.pop();
                            blocks.top()->append(curblock.cast<ASTNode>());
                            curblock = blocks.top();
                        }
                    } else {
                        curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
                    }

                    /* We're in a loop, this jumps back to the start */
                    /* I think we'll just ignore this case... */
                    break; // Bad idea? Probably!
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (cont->hasExcept() && pos < cont->except()) {
                        PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, 0, NULL, false);
                        except->init();
                        blocks.push(except);
                        curblock = blocks.top();
                    }
                    break;
                }

                stack = stack_hist.top();
                stack_hist.pop();

                PycRef<ASTBlock> prev = curblock;
                PycRef<ASTBlock> nil;
                bool push = true;

                do {
                    blocks.pop();

                    blocks.top()->append(prev.cast<ASTNode>());

                    if (prev->blktype() == ASTBlock::BLK_IF
                            || prev->blktype() == ASTBlock::BLK_ELIF) {
                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTBlock(ASTBlock::BLK_ELSE, blocks.top()->end());
                        if (prev->inited() == ASTCondBlock::PRE_POPPED) {
                            next->init(ASTCondBlock::PRE_POPPED);
                        }

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_EXCEPT) {
                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTCondBlock(ASTBlock::BLK_EXCEPT, blocks.top()->end(), NULL, false);
                        next->init();

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_ELSE) {
                        /* Special case */
                        prev = blocks.top();
                        if (!push) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                        push = false;
                    } else {
                        prev = nil;
                    }

                } while (prev != nil);

                curblock = blocks.top();
            }
            break;
        
        
        case Pyc::JUMP_FORWARD_A:
        case Pyc::INSTRUMENTED_JUMP_FORWARD_A:
            {
                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // BPO-27129

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (cont->hasExcept()) {
                        stack_hist.push(stack);

                        curblock->setEnd(pos + offs);
                        PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos + offs, NULL, false);
                        except->init();
                        blocks.push(except);
                        curblock = blocks.top();
                    }
                    break;
                }

                if (!stack_hist.empty()) {
                    if (stack.empty()) // if it's part of if-expression, TOS at the moment is the result of "if" part
                        stack = stack_hist.top();
                    stack_hist.pop();
                }

                if (blocks.size() == 1 && !source.atEof()) {
                    fprintf(stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", pos, opcode & 0xff);
                    cleanBuild = false;
                    break;
                }

                PycRef<ASTBlock> prev = curblock;
                PycRef<ASTBlock> nil;
                bool push = true;

                // Spam prevention counter and threshold.
                int spamCounter = 0;
                const int spamThreshold = 100; // Adjust as necessary.

                do {
                    if (++spamCounter > spamThreshold) {
                        // When the threshold is exceeded, exit the loop silently.
                        break;
                    }

                    blocks.pop();

                    if (!blocks.empty())
                        blocks.top()->append(prev.cast<ASTNode>());

                    if (prev->blktype() == ASTBlock::BLK_IF || prev->blktype() == ASTBlock::BLK_ELIF) {
                        if (offs == 0) {
                            prev = nil;
                            continue;
                        }

                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTBlock(ASTBlock::BLK_ELSE, pos + offs);
                        if (prev->inited() == ASTCondBlock::PRE_POPPED) {
                            next->init(ASTCondBlock::PRE_POPPED);
                        }

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_EXCEPT) {
                        if (offs == 0) {
                            prev = nil;
                            continue;
                        }

                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos + offs, NULL, false);
                        next->init();

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_ELSE) {
                        /* Special case */
                        prev = blocks.top();
                        if (!push) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                        push = false;

                        if (prev->blktype() == ASTBlock::BLK_MAIN) {
                            /* Something went out of control! */
                            prev = nil;
                        }
                    } else if (prev->blktype() == ASTBlock::BLK_TRY && prev->end() < pos + offs) {
                        /* Need to add an except/finally block */
                        stack = stack_hist.top();
                        stack.pop();
                        if (blocks.top()->blktype() == ASTBlock::BLK_CONTAINER) {
                            PycRef<ASTContainerBlock> cont = blocks.top().cast<ASTContainerBlock>();
                            if (cont->hasExcept()) {
                                if (push) {
                                    stack_hist.push(stack);
                                }
                                PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos + offs, NULL, false);
                                except->init();
                                blocks.push(except);
                            }
                        } else {
                            fprintf(stderr, "Something TERRIBLE happened!!\n");
                        }
                        prev = nil;
                    } else {
                        prev = nil;
                    }
                } while (prev != nil);

                if (blocks.size() > 0) {
                    curblock = blocks.top();
                    if (curblock && curblock->blktype() == ASTBlock::BLK_EXCEPT) {
                        curblock->setEnd(pos + offs);
                    }
                } else if (curblock) {
                    curblock->setEnd(pos + offs);
                }
            }
            break;                                        
        case Pyc::LIST_APPEND:
        case Pyc::LIST_APPEND_A:
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                PycRef<ASTNode> list = stack.top();


                if (curblock->blktype() == ASTBlock::BLK_FOR
                        && curblock.cast<ASTIterBlock>()->isComprehension()) {
                    stack.pop();
                    stack.push(new ASTComprehension(value));
                } else {
                    stack.push(new ASTSubscr(list, value)); /* Total hack */
                }
            }
            break;
        case Pyc::SET_UPDATE_A:
            {
                PycRef<ASTNode> rhs = stack.top();
                stack.pop();
                PycRef<ASTSet> lhs = stack.top().cast<ASTSet>();
                stack.pop();

                if (rhs.type() != ASTNode::NODE_OBJECT) {
                    fprintf(stderr, "Unsupported argument found for SET_UPDATE\n");
                    break;
                }

                // I've only ever seen this be a TYPE_FROZENSET, but let's be careful...
                PycRef<PycObject> obj = rhs.cast<ASTObject>()->object();
                if (obj->type() != PycObject::TYPE_FROZENSET) {
                    fprintf(stderr, "Unsupported argument type found for SET_UPDATE\n");
                    break;
                }

                ASTSet::value_t result = lhs->values();
                for (const auto& it : obj.cast<PycSet>()->values()) {
                    result.push_back(new ASTObject(it));
                }

                stack.push(new ASTSet(result));
            }
            break;
        case Pyc::LIST_EXTEND_A:
            {
                PycRef<ASTNode> rhs = stack.top();
                stack.pop();
                PycRef<ASTList> lhs = stack.top().cast<ASTList>();
                stack.pop();

                if (rhs.type() != ASTNode::NODE_OBJECT) {
                    fprintf(stderr, "Unsupported argument found for LIST_EXTEND\n");
                    break;
                }

                // I've only ever seen this be a SMALL_TUPLE, but let's be careful...
                PycRef<PycObject> obj = rhs.cast<ASTObject>()->object();
                if (obj->type() != PycObject::TYPE_TUPLE && obj->type() != PycObject::TYPE_SMALL_TUPLE) {
                    fprintf(stderr, "Unsupported argument type found for LIST_EXTEND\n");
                    break;
                }

                ASTList::value_t result = lhs->values();
                for (const auto& it : obj.cast<PycTuple>()->values()) {
                    result.push_back(new ASTObject(it));
                }

                stack.push(new ASTList(result));
            }
            break;
        case Pyc::LOAD_ATTR_A:
            {
                PycRef<ASTNode> name = stack.top();
                if (name.type() != ASTNode::NODE_IMPORT) {
                    stack.pop();

                    if (mod->verCompare(3, 12) >= 0) {
                        if (operand & 1) {
                            /* Changed in version 3.12:
                            If the low bit of name is set, then a NULL or self is pushed to the stack
                            before the attribute or unbound method respectively. */
                            stack.push(nullptr);
                        }
                        operand >>= 1;
                    }

                    stack.push(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR));
                }
            }
            break;
        case Pyc::LOAD_BUILD_CLASS:
            stack.push(new ASTLoadBuildClass(new PycObject()));
            break;
        case Pyc::LOAD_CLOSURE_A:
            /* Ignore this */
            break;
        case Pyc::LOAD_CONST_A:
            {
                PycRef<ASTObject> t_ob = new ASTObject(code->getConst(operand));

                if ((t_ob->object().type() == PycObject::TYPE_TUPLE ||
                        t_ob->object().type() == PycObject::TYPE_SMALL_TUPLE) &&
                        !t_ob->object().cast<PycTuple>()->values().size()) {
                    ASTTuple::value_t values;
                    stack.push(new ASTTuple(values));
                } else if (t_ob->object().type() == PycObject::TYPE_NONE) {
                    stack.push(NULL);
                } else {
                    stack.push(t_ob.cast<ASTNode>());
                }
            }
            break;
        case Pyc::LOAD_SMALL_INT_A:
            /* Python 3.14+: push a small integer literal in range(256) */
            stack.push(new ASTObject(new PycInt(operand)));
            break;
        case Pyc::LOAD_DEREF_A:
        case Pyc::LOAD_CLASSDEREF_A:
            stack.push(new ASTName(code->getCellVar(mod, operand)));
            break;
        case Pyc::LOAD_FAST_A:
        case Pyc::LOAD_FAST_CHECK_A:
        case Pyc::LOAD_FAST_BORROW_A:
            if (mod->verCompare(1, 3) < 0)
                stack.push(new ASTName(code->getName(operand)));
            else
                stack.push(new ASTName(code->getLocal(operand)));
            break;
        case Pyc::LOAD_FAST_LOAD_FAST_A:
        case Pyc::LOAD_FAST_BORROW_LOAD_FAST_BORROW_A:
            stack.push(new ASTName(code->getLocal(operand >> 4)));
            stack.push(new ASTName(code->getLocal(operand & 0xF)));
            break;
        case Pyc::LOAD_GLOBAL_A:
            if (mod->verCompare(3, 11) >= 0) {
                // Loads the global named co_names[namei>>1] onto the stack.
                if (operand & 1) {
                    /* Changed in version 3.11: 
                    If the low bit of "NAMEI" (operand) is set, 
                    then a NULL is pushed to the stack before the global variable. */
                    stack.push(nullptr);
                }
                operand >>= 1;
            }
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::LOAD_LOCALS:
            stack.push(new ASTNode(ASTNode::NODE_LOCALS));
            break;
        case Pyc::STORE_LOCALS:
            stack.pop();
            break;
        case Pyc::LOAD_METHOD_A:
            {
                // Behave like LOAD_ATTR
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                stack.push(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR));
            }
            break;
        case Pyc::LOAD_NAME_A:
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::MAKE_CLOSURE_A:
        case Pyc::MAKE_FUNCTION:
        case Pyc::MAKE_FUNCTION_A:
            {
                PycRef<ASTNode> fun_code = stack.top();
                stack.pop();
                // Test for the qualified name of the function (at TOS)
                int tos_type = fun_code.cast<ASTObject>()->object().type();
                if (tos_type != PycObject::TYPE_CODE &&
                    tos_type != PycObject::TYPE_CODE2) {
                    fun_code = stack.top();
                    stack.pop();
                }

                ASTFunction::defarg_t defArgs, kwDefArgs;
                const int defCount = operand & 0xFF;
                const int kwDefCount = (operand >> 8) & 0xFF;
                const int annotationCount = (operand >> 16) & 0x7FFF;

                // Python 3.3 and below
                if (mod->verCompare(3, 3) <= 0) {
                    for (int i = 0; i < defCount; ++i) {
                        defArgs.push_front(stack.top());
                        stack.pop();
                    }
                    // KW Defaults not mentioned in docs, but they come after the positional args
                    for (int i = 0; i < kwDefCount; ++i) {
                        kwDefArgs.push_front(stack.top());
                        stack.pop(); // KW Pair object
                        stack.pop(); // KW Pair name
                    }
                }
                // Python 3.4-3.5
                else if (mod->verCompare(3, 5) <= 0) {
                    // From Py 3.4: annotations and order change (kw first)
                    if (annotationCount) {
                        stack.pop(); // Tuple of param names for annotations
                        for (int i = 0; i < annotationCount; ++i) {
                            stack.pop(); // Pop annotation objects and ignore
                        }
                    }
                    for (int i = 0; i < kwDefCount; ++i) {
                        kwDefArgs.push_front(stack.top());
                        stack.pop(); // KW Pair object
                        stack.pop(); // KW Pair name
                    }
                    for (int i = 0; i < defCount; ++i) {
                        defArgs.push_front(stack.top());
                        stack.pop();
                    }
                }
                // Python 3.6-3.9 (flag mask, annotation dict)
                else if (mod->verCompare(3, 9) <= 0) {
                    if (operand & 0x08) { // Cells for free vars to create a closure
                        stack.pop(); // Ignore these for syntax generation
                    }
                    if (operand & 0x04) { // Annotation dict (3.6-9)
                        stack.pop(); // Ignore annotations
                    }
                    if (operand & 0x02) { // Kwarg Defaults
                        PycRef<ASTNode> kw_tuple = stack.top();
                        stack.pop();
                        std::vector<PycRef<ASTNode>> kw_values = kw_tuple.cast<ASTConstMap>()->values();
                        for (const PycRef<ASTNode>& kw : kw_values) {
                            kwDefArgs.push_front(kw);
                        }
                    }
                    if (operand & 0x01) { // Positional Defaults (including positional-or-KW args)
                        PycRef<ASTNode> pos_tuple = stack.top();
                        stack.pop();
                        std::vector<PycRef<PycObject>> pos_values = pos_tuple.cast<ASTObject>()->object().cast<PycTuple>()->values();
                        for (const PycRef<PycObject>& pos : pos_values) {
                            defArgs.push_back(new ASTObject(pos));
                        }
                    }
                }
                // Python 3.10-3.13 (annotation can be dict or string, otherwise same)
                else if (mod->verCompare(3, 13) <= 0) {
                    if (operand & 0x08) { // Cells for free vars to create a closure
                        stack.pop(); // Ignore these for syntax generation
                    }
                    if (operand & 0x04) { // Annotation dict (3.10-13) or string (PEP 563)
                        stack.pop(); // Ignore annotations
                    }
                    if (operand & 0x02) { // Kwarg Defaults
                        PycRef<ASTNode> kw_tuple = stack.top();
                        stack.pop();
                        std::vector<PycRef<ASTNode>> kw_values = kw_tuple.cast<ASTConstMap>()->values();
                        for (const PycRef<ASTNode>& kw : kw_values) {
                            kwDefArgs.push_front(kw);
                        }
                    }
                    if (operand & 0x01) { // Positional Defaults (including positional-or-KW args)
                        PycRef<ASTNode> pos_tuple = stack.top();
                        stack.pop();
                        std::vector<PycRef<PycObject>> pos_values = pos_tuple.cast<ASTObject>()->object().cast<PycTuple>()->values();
                        for (const PycRef<PycObject>& pos : pos_values) {
                            defArgs.push_back(new ASTObject(pos));
                        }
                    }
                }
                // Python 3.14+ (assume new flag 0x10 for environment dict, otherwise same as 3.13)
                else if (mod->verCompare(3, 14) <= 0) {
                    if (operand & 0x10) { // Hypothetical: function environment variables
                        stack.pop(); // Ignore environment dict
                    }
                    if (operand & 0x08) { // Cells for free vars to create a closure
                        stack.pop(); // Ignore these for syntax generation
                    }
                    if (operand & 0x04) { // Annotation dict or string
                        stack.pop(); // Ignore annotations
                    }
                    if (operand & 0x02) { // Kwarg Defaults
                        PycRef<ASTNode> kw_tuple = stack.top();
                        stack.pop();
                        std::vector<PycRef<ASTNode>> kw_values = kw_tuple.cast<ASTConstMap>()->values();
                        for (const PycRef<ASTNode>& kw : kw_values) {
                            kwDefArgs.push_front(kw);
                        }
                    }
                    if (operand & 0x01) { // Positional Defaults (including positional-or-KW args)
                        PycRef<ASTNode> pos_tuple = stack.top();
                        stack.pop();
                        std::vector<PycRef<PycObject>> pos_values = pos_tuple.cast<ASTObject>()->object().cast<PycTuple>()->values();
                        for (const PycRef<PycObject>& pos : pos_values) {
                            defArgs.push_back(new ASTObject(pos));
                        }
                    }
                }
                // Unknown future versions: fallback, try 3.14 logic
                else {
                    if (operand & 0x10) {
                        stack.pop();
                    }
                    if (operand & 0x08) {
                        stack.pop();
                    }
                    if (operand & 0x04) {
                        stack.pop();
                    }
                    if (operand & 0x02) {
                        PycRef<ASTNode> kw_tuple = stack.top();
                        stack.pop();
                        std::vector<PycRef<ASTNode>> kw_values = kw_tuple.cast<ASTConstMap>()->values();
                        for (const PycRef<ASTNode>& kw : kw_values) {
                            kwDefArgs.push_front(kw);
                        }
                    }
                    if (operand & 0x01) {
                        PycRef<ASTNode> pos_tuple = stack.top();
                        stack.pop();
                        std::vector<PycRef<PycObject>> pos_values = pos_tuple.cast<ASTObject>()->object().cast<PycTuple>()->values();
                        for (const PycRef<PycObject>& pos : pos_values) {
                            defArgs.push_back(new ASTObject(pos));
                        }
                    }
                }

                stack.push(new ASTFunction(fun_code, defArgs, kwDefArgs));
            }
            break;
        case Pyc::NOP:
            break;
        case Pyc::POP_BLOCK:
            {
                if (curblock->blktype() == ASTBlock::BLK_CONTAINER ||
                        curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    /* These should only be popped by an END_FINALLY */
                    break;
                }

                if (curblock->blktype() == ASTBlock::BLK_WITH) {
                    // This should only be popped by a WITH_CLEANUP
                    break;
                }

                if (curblock->nodes().size() &&
                        curblock->nodes().back().type() == ASTNode::NODE_KEYWORD) {
                    curblock->removeLast();
                }

                if (blocks.size() == 1 && !source.atEof()) {
                    fprintf(stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", pos, opcode & 0xff);
                    cleanBuild = false;
                    break;
                }
                    
                    

                if (curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELIF
                        || curblock->blktype() == ASTBlock::BLK_ELSE
                        || curblock->blktype() == ASTBlock::BLK_TRY
                        || curblock->blktype() == ASTBlock::BLK_EXCEPT
                        || curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    if (!stack_hist.empty()) {
                        stack = stack_hist.top();
                        stack_hist.pop();
                    } else {
                        fprintf(stderr, "Warning: Stack history is empty, something wrong might have happened\n");
                    }
                }
                PycRef<ASTBlock> tmp = curblock;
                blocks.pop();

                if (!blocks.empty())
                    curblock = blocks.top();

                if (!(tmp->blktype() == ASTBlock::BLK_ELSE
                        && tmp->nodes().size() == 0)) {
                    curblock->append(tmp.cast<ASTNode>());
                }

                if (tmp->blktype() == ASTBlock::BLK_FOR && tmp->end() >= pos) {
                    stack_hist.push(stack);

                    PycRef<ASTBlock> blkelse = new ASTBlock(ASTBlock::BLK_ELSE, tmp->end());
                    blocks.push(blkelse);
                    curblock = blocks.top();
                }

                if (curblock->blktype() == ASTBlock::BLK_TRY
                        && tmp->blktype() != ASTBlock::BLK_FOR
                        && tmp->blktype() != ASTBlock::BLK_ASYNCFOR
                        && tmp->blktype() != ASTBlock::BLK_WHILE) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    tmp = curblock;
                    blocks.pop();
                    curblock = blocks.top();

                    if (!(tmp->blktype() == ASTBlock::BLK_ELSE
                            && tmp->nodes().size() == 0)) {
                        curblock->append(tmp.cast<ASTNode>());
                    }
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();

                    if (tmp->blktype() == ASTBlock::BLK_ELSE && !cont->hasFinally()) {

                        /* Pop the container */
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(cont.cast<ASTNode>());

                    } else if ((tmp->blktype() == ASTBlock::BLK_ELSE && cont->hasFinally())
                            || (tmp->blktype() == ASTBlock::BLK_TRY && !cont->hasExcept())) {

                        /* Add the finally block */
                        stack_hist.push(stack);

                        PycRef<ASTBlock> final = new ASTBlock(ASTBlock::BLK_FINALLY, 0, true);
                        blocks.push(final);
                        curblock = blocks.top();
                    }
                }

                if ((curblock->blktype() == ASTBlock::BLK_FOR || curblock->blktype() == ASTBlock::BLK_ASYNCFOR)
                        && curblock->end() == pos) {
                    blocks.pop();
                    blocks.top()->append(curblock.cast<ASTNode>());
                    curblock = blocks.top();
                }
            }
            break;
        case Pyc::POP_EXCEPT:
            /* Python 3.11+: closes the current except clause.  Pop the BLK_EXCEPT
               back into its container so the next clause (or the end of the
               handler) is processed against the container again. */
            if (zce_active_handler >= 0
                    && curblock->blktype() == ASTBlock::BLK_EXCEPT) {
                PycRef<ASTBlock> except = curblock;
                blocks.pop();
                curblock = blocks.top();
                curblock->append(except.cast<ASTNode>());
                if (!stack_hist.empty()) {
                    stack = stack_hist.top();
                    stack_hist.pop();
                }
            }
            break;
        case Pyc::POP_ITER:
            /* Python 3.14+: pops the exhausted iterator after a for loop.
               In our stack model FOR_ITER already consumed the iterator, so
               there is nothing to do here (END_FOR closed the block). */
            break;
        case Pyc::NOT_TAKEN:
            /* Python 3.14+: pure branch-prediction hint, no stack or code effect.
               Inside a zero-cost handler, NOT_TAKEN immediately precedes either a
               POP_TOP (bare except) or a STORE_FAST that binds the `as <name>`
               alias.  Flag the latter so the following store is treated as the
               alias rather than a normal assignment. */
            if (zce_active_handler >= 0
                    && curblock->blktype() == ASTBlock::BLK_EXCEPT)
                zce_expect_as = true;
            break;
        case Pyc::END_FOR:
            {
                stack.pop();

                if ((opcode == Pyc::END_FOR) && (mod->majorVer() == 3) && (mod->minorVer() == 12)) {
                    // one additional pop for python 3.12
                    stack.pop();
                }

                // end for loop here
                /* TODO : Ensure that FOR loop ends here. 
                   Due to CACHE instructions at play, the end indicated in
                   the for loop by pycdas is not correct, it is off by
                   some small amount. */
                if (curblock->blktype() == ASTBlock::BLK_FOR) {
                    PycRef<ASTBlock> prev = blocks.top();
                    blocks.pop();

                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());
                }
                else {
                    fprintf(stderr, "Wrong block type %i for END_FOR\n", curblock->blktype());
                }
            }
            break;
        case Pyc::POP_TOP:
            {
                /* Python 3.11+: a POP_TOP right after PUSH_EXC_INFO (bare except:)
                   or after NOT_TAKEN (except <type>: with no `as` alias). */
                if (zce_expect_check) {
                    /* Bare except: — no CHECK_EXC_MATCH, immediate POP_TOP.
                       Create an unconditional except block (NULL cond renders as
                       a bare `except:`). */
                    zce_expect_check = false;
                    stack.pop();  // Discard the exception placeholder
                    PycRef<ASTBlock> except_block =
                            new ASTCondBlock(ASTBlock::BLK_EXCEPT, curpos, NULL, false);
                    except_block->init();
                    blocks.push(except_block);
                    curblock = blocks.top();
                    break;
                }
                if (zce_expect_as) {
                    zce_expect_as = false;
                    stack.pop();
                    break;
                }
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                if (!curblock->inited()) {
                    if (curblock->blktype() == ASTBlock::BLK_WITH) {
                        curblock.cast<ASTWithBlock>()->setExpr(value);
                    } else {
                        curblock->init();
                    }
                    break;
                } else if (value == nullptr || value->processed()) {
                    break;
                }

                curblock->append(value);

                if (curblock->blktype() == ASTBlock::BLK_FOR
                        && curblock.cast<ASTIterBlock>()->isComprehension()) {
                    /* This relies on some really uncertain logic...
                     * If it's a comprehension, the only POP_TOP should be
                     * a call to append the iter to the list.
                     */
                    if (value.type() == ASTNode::NODE_CALL) {
                        auto& pparams = value.cast<ASTCall>()->pparams();
                        if (!pparams.empty()) {
                            PycRef<ASTNode> res = pparams.front();
                            stack.push(new ASTComprehension(res));
                        }
                    }
                }
            }
            break;
        case Pyc::PRINT_ITEM:
            {
                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == nullptr && !printNode->eol())
                    printNode->add(stack.top());
                else
                    curblock->append(new ASTPrint(stack.top()));
                stack.pop();
            }
            break;
        case Pyc::PRINT_ITEM_TO:
            {
                PycRef<ASTNode> stream = stack.top();
                stack.pop();

                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == stream && !printNode->eol())
                    printNode->add(stack.top());
                else
                    curblock->append(new ASTPrint(stack.top(), stream));
                stack.pop();
                stream->setProcessed();
            }
            break;
        case Pyc::PRINT_NEWLINE:
            {
                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == nullptr && !printNode->eol())
                    printNode->setEol(true);
                else
                    curblock->append(new ASTPrint(nullptr));
                stack.pop();
            }
            break;
        case Pyc::PRINT_NEWLINE_TO:
            {
                PycRef<ASTNode> stream = stack.top();
                stack.pop();

                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == stream && !printNode->eol())
                    printNode->setEol(true);
                else
                    curblock->append(new ASTPrint(nullptr, stream));
                stack.pop();
                stream->setProcessed();
            }
            break;

        case Pyc::RAISE_VARARGS_A:
            {
                ASTRaise::param_t paramList;
                for (int i = 0; i < operand; i++) {
                    paramList.push_front(stack.top());
                    stack.pop();
                }
                curblock->append(new ASTRaise(paramList));

                if ((curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELSE)
                        && stack_hist.size()
                        && (mod->verCompare(2, 6) >= 0)) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    PycRef<ASTBlock> prev = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());
                }
            }
            break;


        case Pyc::RETURN_VALUE:
        case Pyc::INSTRUMENTED_RETURN_VALUE_A:
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                curblock->append(new ASTReturn(value));

                if ((curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELSE)
                        && stack_hist.size()
                        && (mod->verCompare(2, 6) >= 0)) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    PycRef<ASTBlock> prev = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());
                    bc_next(source, mod, opcode, operand, pos);
                }
            }
            break;
        case Pyc::RETURN_CONST_A:
        case Pyc::INSTRUMENTED_RETURN_CONST_A:
            {
                PycRef<ASTObject> value = new ASTObject(code->getConst(operand));
                curblock->append(new ASTReturn(value.cast<ASTNode>()));
            }
            break;
        case Pyc::ROT_TWO:
            {
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> two = stack.top();
                stack.pop();

                stack.push(one);
                stack.push(two);
            }
            break;
        case Pyc::ROT_THREE:
            {
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                PycRef<ASTNode> two = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> three = stack.top();
                stack.pop();
                stack.push(one);
                stack.push(three);
                stack.push(two);
            }
            break;
        case Pyc::ROT_FOUR:
            {
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                PycRef<ASTNode> two = stack.top();
                stack.pop();
                PycRef<ASTNode> three = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> four = stack.top();
                stack.pop();
                stack.push(one);
                stack.push(four);
                stack.push(three);
                stack.push(two);
            }
            break;
        case Pyc::SET_LINENO_A:
            // Ignore
            break;
        case Pyc::SETUP_WITH_A:
        case Pyc::WITH_EXCEPT_START:
            {
                PycRef<ASTBlock> withblock = new ASTWithBlock(pos+operand);
                blocks.push(withblock);
                curblock = blocks.top();
            }
            break;
        case Pyc::WITH_CLEANUP:
        case Pyc::WITH_CLEANUP_START:
            {
                // Stack top should be a None. Ignore it.
                PycRef<ASTNode> none = stack.top();
                stack.pop();

                if (none != NULL) {
                    fprintf(stderr, "Something TERRIBLE happened!\n");
                    break;
                }

                if (curblock->blktype() == ASTBlock::BLK_WITH
                        && curblock->end() == curpos) {
                    PycRef<ASTBlock> with = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(with.cast<ASTNode>());
                }
                else {
                    fprintf(stderr, "Something TERRIBLE happened! No matching with block found for WITH_CLEANUP at %d\n", curpos);
                }
            }
            break;
        case Pyc::WITH_CLEANUP_FINISH:
            /* Ignore this */
            break;
        case Pyc::SETUP_EXCEPT_A:
            {
                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    curblock.cast<ASTContainerBlock>()->setExcept(pos+operand);
                } else {
                    PycRef<ASTBlock> next = new ASTContainerBlock(0, pos+operand);
                    blocks.push(next.cast<ASTBlock>());
                }

                /* Store the current stack for the except/finally statement(s) */
                stack_hist.push(stack);
                PycRef<ASTBlock> tryblock = new ASTBlock(ASTBlock::BLK_TRY, pos+operand, true);
                blocks.push(tryblock.cast<ASTBlock>());
                curblock = blocks.top();

                need_try = false;
            }
            break;
        case Pyc::SETUP_FINALLY_A:
            {
                PycRef<ASTBlock> next = new ASTContainerBlock(pos+operand);
                blocks.push(next.cast<ASTBlock>());
                curblock = blocks.top();

                need_try = true;
            }
            break;
        case Pyc::SETUP_LOOP_A:
            {
                PycRef<ASTBlock> next = new ASTCondBlock(ASTBlock::BLK_WHILE, pos+operand, NULL, false);
                blocks.push(next.cast<ASTBlock>());
                curblock = blocks.top();
            }
            break;
        case Pyc::SLICE_0:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                PycRef<ASTNode> slice = new ASTSlice(ASTSlice::SLICE0);
                stack.push(new ASTSubscr(name, slice));
            }
            break;
        case Pyc::SLICE_1:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                PycRef<ASTNode> slice = new ASTSlice(ASTSlice::SLICE1, lower);
                stack.push(new ASTSubscr(name, slice));
            }
            break;
        case Pyc::SLICE_2:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                PycRef<ASTNode> slice = new ASTSlice(ASTSlice::SLICE2, NULL, upper);
                stack.push(new ASTSubscr(name, slice));
            }
            break;
        case Pyc::SLICE_3:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                PycRef<ASTNode> slice = new ASTSlice(ASTSlice::SLICE3, lower, upper);
                stack.push(new ASTSubscr(name, slice));
            }
            break;
        case Pyc::STORE_ATTR_A:
            {
                if (unpack) {
                    PycRef<ASTNode> name = stack.top();
                    stack.pop();
                    PycRef<ASTNode> attr = new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR);

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(attr);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();
                        if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> name = stack.top();
                    stack.pop();
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<ASTNode> attr = new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR);
                    if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, attr, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, attr));
                    }
                }
            }
            break;
        case Pyc::STORE_DEREF_A:
            {
                if (unpack) {
                    PycRef<ASTNode> name = new ASTName(code->getCellVar(mod, operand));

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(name);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();

                        if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<ASTNode> name = new ASTName(code->getCellVar(mod, operand));

                    if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, name, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, name));
                    }
                }
            }
            break;
        case Pyc::STORE_FAST_A:
            {
                /* Python 3.11+ zero-cost exceptions: capture the `except ... as
                   <name>` alias, and suppress the compiler-generated
                   `<name> = None; del <name>` cleanup emitted at clause end. */
                if (zce_active_handler >= 0) {
                    PycRef<PycString> local = (mod->verCompare(1, 3) < 0)
                            ? code->getName(operand) : code->getLocal(operand);
                    if (zce_expect_as
                            && curblock->blktype() == ASTBlock::BLK_EXCEPT) {
                        zce_expect_as = false;
                        zce_as_name = local;
                        curblock.cast<ASTCondBlock>()->setExceptAs(new ASTName(local));
                        stack.pop(); // consume the exception value being bound
                        break;
                    }
                    if (zce_as_name != NULL && local->isEqual(zce_as_name->strValue())) {
                        PycRef<ASTNode> value = stack.top();
                        // `<name> = None` cleanup -> drop it silently.
                        if (value == NULL || (value.type() == ASTNode::NODE_OBJECT
                                && value.cast<ASTObject>()->object().type() == PycObject::TYPE_NONE)) {
                            stack.pop();
                            break;
                        }
                    }
                }
                if (unpack) {
                    PycRef<ASTNode> name;

                    if (mod->verCompare(1, 3) < 0)
                        name = new ASTName(code->getName(operand));
                    else
                        name = new ASTName(code->getLocal(operand));

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(name);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();

                        if (curblock->blktype() == ASTBlock::BLK_FOR
                                && !curblock->inited()) {
                            PycRef<ASTTuple> tuple = tup.try_cast<ASTTuple>();
                            if (tuple != NULL)
                                tuple->setRequireParens(false);
                            curblock.cast<ASTIterBlock>()->setIndex(tup);
                        } else if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<ASTNode> name;

                    if (mod->verCompare(1, 3) < 0)
                        name = new ASTName(code->getName(operand));
                    else
                        name = new ASTName(code->getLocal(operand));

                    if (name.cast<ASTName>()->name()->value()[0] == '_'
                            && name.cast<ASTName>()->name()->value()[1] == '[') {
                        /* Don't show stores of list comp append objects. */
                        break;
                    }

                    if (curblock->blktype() == ASTBlock::BLK_FOR
                            && !curblock->inited()) {
                        curblock.cast<ASTIterBlock>()->setIndex(name);
                    } else if (curblock->blktype() == ASTBlock::BLK_WITH
                                   && !curblock->inited()) {
                        curblock.cast<ASTWithBlock>()->setExpr(value);
                        curblock.cast<ASTWithBlock>()->setVar(name);
                    } else if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, name, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, name));
                    }
                }
            }
            break;
        case Pyc::STORE_GLOBAL_A:
            {
                PycRef<ASTNode> name = new ASTName(code->getName(operand));

                if (unpack) {
                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(name);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();

                        if (curblock->blktype() == ASTBlock::BLK_FOR
                                && !curblock->inited()) {
                            PycRef<ASTTuple> tuple = tup.try_cast<ASTTuple>();
                            if (tuple != NULL)
                                tuple->setRequireParens(false);
                            curblock.cast<ASTIterBlock>()->setIndex(tup);
                        } else if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, name, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, name));
                    }
                }

                /* Mark the global as used */
                code->markGlobal(name.cast<ASTName>()->name());
            }
            break;
        case Pyc::STORE_NAME_A:
            {
                if (unpack) {
                    PycRef<ASTNode> name = new ASTName(code->getName(operand));

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(name);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();

                        if (curblock->blktype() == ASTBlock::BLK_FOR
                                && !curblock->inited()) {
                            PycRef<ASTTuple> tuple = tup.try_cast<ASTTuple>();
                            if (tuple != NULL)
                                tuple->setRequireParens(false);
                            curblock.cast<ASTIterBlock>()->setIndex(tup);
                        } else if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();

                    PycRef<PycString> varname = code->getName(operand);
                    if (varname->length() >= 2 && varname->value()[0] == '_'
                            && varname->value()[1] == '[') {
                        /* Don't show stores of list comp append objects. */
                        break;
                    }

                    // Return private names back to their original name
                    const std::string class_prefix = std::string("_") + code->name()->strValue();
                    if (varname->startsWith(class_prefix + std::string("__")))
                        varname->setValue(varname->strValue().substr(class_prefix.size()));

                    PycRef<ASTNode> name = new ASTName(varname);

                    if (curblock->blktype() == ASTBlock::BLK_FOR
                            && !curblock->inited()) {
                        curblock.cast<ASTIterBlock>()->setIndex(name);
                    } else if (stack.top().type() == ASTNode::NODE_IMPORT) {
                        PycRef<ASTImport> import = stack.top().cast<ASTImport>();

                        import->add_store(new ASTStore(value, name));
                    } else if (curblock->blktype() == ASTBlock::BLK_WITH
                               && !curblock->inited()) {
                        curblock.cast<ASTWithBlock>()->setExpr(value);
                        curblock.cast<ASTWithBlock>()->setVar(name);
                    } else if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, name, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, name));

                        if (value.type() == ASTNode::NODE_INVALID)
                            break;
                    }
                }
            }
            break;
        case Pyc::STORE_SLICE_0:
            {
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                curblock->append(new ASTStore(value, new ASTSubscr(dest, new ASTSlice(ASTSlice::SLICE0))));
            }
            break;
        case Pyc::STORE_SLICE_1:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                curblock->append(new ASTStore(value, new ASTSubscr(dest, new ASTSlice(ASTSlice::SLICE1, upper))));
            }
            break;
        case Pyc::STORE_SLICE_2:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                curblock->append(new ASTStore(value, new ASTSubscr(dest, new ASTSlice(ASTSlice::SLICE2, NULL, lower))));
            }
            break;
        case Pyc::STORE_SLICE_3:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                curblock->append(new ASTStore(value, new ASTSubscr(dest, new ASTSlice(ASTSlice::SLICE3, upper, lower))));
            }
            break;
        case Pyc::STORE_SUBSCR:
            {
                if (unpack) {
                    PycRef<ASTNode> subscr = stack.top();
                    stack.pop();
                    PycRef<ASTNode> dest = stack.top();
                    stack.pop();

                    PycRef<ASTNode> save = new ASTSubscr(dest, subscr);

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(save);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();
                        if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> subscr = stack.top();
                    stack.pop();
                    PycRef<ASTNode> dest = stack.top();
                    stack.pop();
                    PycRef<ASTNode> src = stack.top();
                    stack.pop();

                    // If variable annotations are enabled, we'll need to check for them here.
                    // Python handles a varaible annotation by setting:
                    // __annotations__['var-name'] = type
                    const bool found_annotated_var = (variable_annotations && dest->type() == ASTNode::Type::NODE_NAME
                                                      && dest.cast<ASTName>()->name()->isEqual("__annotations__"));

                    if (found_annotated_var) {
                        // Annotations can be done alone or as part of an assignment.
                        // In the case of an assignment, we'll see a NODE_STORE on the stack.
                        if (!curblock->nodes().empty() && curblock->nodes().back()->type() == ASTNode::Type::NODE_STORE) {
                            // Replace the existing NODE_STORE with a new one that includes the annotation.
                            PycRef<ASTStore> store = curblock->nodes().back().cast<ASTStore>();
                            curblock->removeLast();
                            curblock->append(new ASTStore(store->src(),
                                                          new ASTAnnotatedVar(subscr, src)));
                        } else {
                            curblock->append(new ASTAnnotatedVar(subscr, src));
                        }
                    } else {
                        if (dest.type() == ASTNode::NODE_MAP) {
                            dest.cast<ASTMap>()->add(subscr, src);
                        } else if (src.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(src, new ASTSubscr(dest, subscr), stack, curblock);
                        } else {
                            curblock->append(new ASTStore(src, new ASTSubscr(dest, subscr)));
                        }
                    }
                }
            }
            break;
        case Pyc::UNARY_CALL:
            {
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                stack.push(new ASTCall(func, ASTCall::pparam_t(), ASTCall::kwparam_t()));
            }
            break;
        case Pyc::UNARY_CONVERT:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                stack.push(new ASTConvert(name));
            }
            break;
        case Pyc::UNARY_INVERT:
            {
                PycRef<ASTNode> arg = stack.top();
                stack.pop();
                stack.push(new ASTUnary(arg, ASTUnary::UN_INVERT));
            }
            break;
        case Pyc::UNARY_NEGATIVE:
            {
                PycRef<ASTNode> arg = stack.top();
                stack.pop();
                stack.push(new ASTUnary(arg, ASTUnary::UN_NEGATIVE));
            }
            break;
        case Pyc::UNARY_NOT:
            {
                PycRef<ASTNode> arg = stack.top();
                stack.pop();
                stack.push(new ASTUnary(arg, ASTUnary::UN_NOT));
            }
            break;
        case Pyc::UNARY_POSITIVE:
            {
                PycRef<ASTNode> arg = stack.top();
                stack.pop();
                stack.push(new ASTUnary(arg, ASTUnary::UN_POSITIVE));
            }
            break;
        case Pyc::UNPACK_LIST_A:
        case Pyc::UNPACK_TUPLE_A:
        case Pyc::UNPACK_SEQUENCE_A:
            {
                unpack = operand;
                if (unpack > 0) {
                    ASTTuple::value_t vals;
                    stack.push(new ASTTuple(vals));
                } else {
                    // Unpack zero values and assign it to top of stack or for loop variable.
                    // E.g. [] = TOS / for [] in X
                    ASTTuple::value_t vals;
                    auto tup = new ASTTuple(vals);
                    if (curblock->blktype() == ASTBlock::BLK_FOR
                        && !curblock->inited()) {
                        tup->setRequireParens(true);
                        curblock.cast<ASTIterBlock>()->setIndex(tup);
                    } else if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                        auto chainStore = stack.top();
                        stack.pop();
                        append_to_chain_store(chainStore, tup, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(stack.top(), tup));
                        stack.pop();
                    }
                }
            }
            break;
        case Pyc::YIELD_FROM:
            {
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                // TODO: Support yielding into a non-null destination
                PycRef<ASTNode> value = stack.top();
                if (value) {
                    value->setProcessed();
                    curblock->append(new ASTReturn(value, ASTReturn::YIELD_FROM));
                }
            }
            break;
        case Pyc::YIELD_VALUE:
        case Pyc::INSTRUMENTED_YIELD_VALUE_A:
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                curblock->append(new ASTReturn(value, ASTReturn::YIELD));
            }
            break;
        case Pyc::SETUP_ANNOTATIONS:
            variable_annotations = true;
            break;
        case Pyc::PRECALL_A:
        case Pyc::RESUME_A:
        case Pyc::INSTRUMENTED_RESUME_A:
            /* We just entirely ignore this / no-op */
            break;
        case Pyc::CACHE:
            /* These "fake" opcodes are used as placeholders for optimizing
               certain opcodes in Python 3.11+.  Since we have no need for
               that during disassembly/decompilation, we can just treat these
               as no-ops. */
            break;
        case Pyc::PUSH_NULL:
            /* Python 3.14 emits standalone PUSH_NULL after some callable
               loads. Normalize that layout to NULL, callable for CALL. */
            if (mod->verCompare(3, 14) >= 0 && !stack.empty()) {
                PycRef<ASTNode> callable = stack.top();
                stack.pop();
                stack.push(nullptr);
                stack.push(callable);
            } else {
                stack.push(nullptr);
            }
            break;
        case Pyc::GEN_START_A:
            stack.pop();
            break;
        case Pyc::SWAP_A:
            {
                unpack = operand;
                ASTTuple::value_t values;
                ASTTuple::value_t next_tuple;
                values.resize(operand);
                for (int i = 0; i < operand; i++) {
                    values[operand - i - 1] = stack.top();
                    stack.pop();
                }
                auto tup = new ASTTuple(values);
                tup->setRequireParens(false);
                auto next_tup = new ASTTuple(next_tuple);
                next_tup->setRequireParens(false);
                stack.push(tup);
                stack.push(next_tup);
            }
            break;
        case Pyc::BINARY_SLICE:
            {
                PycRef<ASTNode> end = stack.top();
                stack.pop();
                PycRef<ASTNode> start = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();

                if (start.type() == ASTNode::NODE_OBJECT
                        && start.cast<ASTObject>()->object() == Pyc_None) {
                    start = NULL;
                }

                if (end.type() == ASTNode::NODE_OBJECT
                        && end.cast<ASTObject>()->object() == Pyc_None) {
                    end = NULL;
                }

                PycRef<ASTNode> slice;
                if (start == NULL && end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE0);
                } else if (start == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE2, start, end);
                } else if (end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE1, start, end);
                } else {
                    slice = new ASTSlice(ASTSlice::SLICE3, start, end);
                }
                stack.push(new ASTSubscr(dest, slice));
            }
            break;
        case Pyc::STORE_SLICE:
            {
                PycRef<ASTNode> end = stack.top();
                stack.pop();
                PycRef<ASTNode> start = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> values = stack.top();
                stack.pop();

                if (start.type() == ASTNode::NODE_OBJECT
                        && start.cast<ASTObject>()->object() == Pyc_None) {
                    start = NULL;
                }

                if (end.type() == ASTNode::NODE_OBJECT
                        && end.cast<ASTObject>()->object() == Pyc_None) {
                    end = NULL;
                }

                PycRef<ASTNode> slice;
                if (start == NULL && end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE0);
                } else if (start == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE2, start, end);
                } else if (end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE1, start, end);
                } else {
                    slice = new ASTSlice(ASTSlice::SLICE3, start, end);
                }

                curblock->append(new ASTStore(values, new ASTSubscr(dest, slice)));
            }
            break;
        case Pyc::COPY_A:
            {
                PycRef<ASTNode> value = stack.top(operand);
                stack.push(value);
            }
            break;
        case Pyc::BUILD_INTERPOLATION_A:
            {
                /* Python 3.14+ (PEP 750): build a single {expr!conv:spec}
                   interpolation for a t-string. The operand encodes
                     conversion  = operand >> 2   (0=none,1=str,2=repr,3=ascii)
                     has_spec    = operand & 0x01
                   Stack (top first):
                     [format_spec?]  (present when has_spec)
                     source_text     (str constant, discarded — we re-render)
                     value           (the interpolated expression)
                   We reuse ASTFormattedValue so the same rendering path as
                   f-strings emits the {value...} piece. */
                PycRef<ASTNode> format_spec;
                if (operand & 0x01) {
                    format_spec = stack.top();
                    stack.pop();
                }
                /* discard the literal source-text expression string */
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                int flags = (operand >> 2) & 0x03;
                if (format_spec != NULL)
                    flags |= ASTFormattedValue::HAVE_FMT_SPEC;
                stack.push(new ASTFormattedValue(value,
                        static_cast<ASTFormattedValue::ConversionFlag>(flags),
                        format_spec));
            }
            break;
        case Pyc::BUILD_TEMPLATE:
            {
                /* Python 3.14+ (PEP 750): combine the strings tuple and the
                   interpolations tuple into a template literal t"...". Stack:
                     interpolations (tuple of ASTFormattedValue)
                     strings        (tuple of str constants) */
                PycRef<ASTNode> interpolations = stack.top();
                stack.pop();
                PycRef<ASTNode> strings = stack.top();
                stack.pop();

                ASTJoinedStr::value_t pieces;
                ASTTuple::value_t str_vals, interp_vals;
                if (strings.type() == ASTNode::NODE_TUPLE) {
                    str_vals = strings.cast<ASTTuple>()->values();
                } else if (strings.type() == ASTNode::NODE_OBJECT) {
                    /* The literal string parts are usually a single tuple
                       constant (LOAD_CONST). Unpack it into ASTObjects. */
                    PycRef<PycObject> obj = strings.cast<ASTObject>()->object();
                    if (obj.type() == PycObject::TYPE_TUPLE
                            || obj.type() == PycObject::TYPE_SMALL_TUPLE) {
                        for (const auto& v : obj.cast<PycTuple>()->values())
                            str_vals.push_back(new ASTObject(v));
                    }
                }
                if (interpolations.type() == ASTNode::NODE_TUPLE)
                    interp_vals = interpolations.cast<ASTTuple>()->values();
                else if (interpolations.type() != ASTNode::NODE_OBJECT)
                    interp_vals.push_back(interpolations);

                /* Interleave literal strings with interpolations, matching the
                   f-string style: str[0], interp[0], str[1], interp[1], ... */
                size_t si = 0;
                for (const auto& interp : interp_vals) {
                    if (si < str_vals.size())
                        pieces.push_back(str_vals[si++]);
                    pieces.push_back(interp);
                }
                while (si < str_vals.size())
                    pieces.push_back(str_vals[si++]);

                stack.push(new ASTJoinedStr(pieces, /*is_template=*/true));
            }
            break;
        /* Python 3.12+ "specialized"/adaptive opcodes.  CPython only produces
           these at run time after quickening; marshal always writes the
           un-specialized base form to a .pyc, so they never occur in files we
           decompile.  Handle them defensively (as their un-adaptive base op's
           no-op equivalent) so a hand-crafted or instrumented object doesn't
           abort the whole decompilation.  These carry inline-cache args we
           cannot recover, so we simply skip them. */
        case Pyc::BINARY_OP_ADD_FLOAT:
        case Pyc::BINARY_OP_ADD_INT:
        case Pyc::BINARY_OP_ADD_UNICODE:
        case Pyc::BINARY_OP_INPLACE_ADD_UNICODE:
        case Pyc::BINARY_OP_MULTIPLY_FLOAT:
        case Pyc::BINARY_OP_MULTIPLY_INT:
        case Pyc::BINARY_OP_SUBTRACT_FLOAT:
        case Pyc::BINARY_OP_SUBTRACT_INT:
        case Pyc::BINARY_OP_SUBSCR_DICT:
        case Pyc::BINARY_OP_SUBSCR_GETITEM:
        case Pyc::BINARY_OP_SUBSCR_LIST_INT:
        case Pyc::BINARY_OP_SUBSCR_LIST_SLICE:
        case Pyc::BINARY_OP_SUBSCR_STR_INT:
        case Pyc::BINARY_OP_SUBSCR_TUPLE_INT:
        case Pyc::BINARY_OP_EXTEND:
        case Pyc::CALL_ALLOC_AND_ENTER_INIT:
        case Pyc::CALL_BOUND_METHOD_EXACT_ARGS:
        case Pyc::CALL_BOUND_METHOD_GENERAL:
        case Pyc::CALL_BUILTIN_CLASS:
        case Pyc::CALL_BUILTIN_FAST:
        case Pyc::CALL_BUILTIN_FAST_WITH_KEYWORDS:
        case Pyc::CALL_BUILTIN_O:
        case Pyc::CALL_ISINSTANCE:
        case Pyc::CALL_KW_BOUND_METHOD:
        case Pyc::CALL_KW_NON_PY:
        case Pyc::CALL_KW_PY:
        case Pyc::CALL_LEN:
        case Pyc::CALL_LIST_APPEND:
        case Pyc::CALL_METHOD_DESCRIPTOR_FAST:
        case Pyc::CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS:
        case Pyc::CALL_METHOD_DESCRIPTOR_NOARGS:
        case Pyc::CALL_METHOD_DESCRIPTOR_O:
        case Pyc::CALL_NON_PY_GENERAL:
        case Pyc::CALL_PY_EXACT_ARGS:
        case Pyc::CALL_PY_GENERAL:
        case Pyc::CALL_STR_1:
        case Pyc::CALL_TUPLE_1:
        case Pyc::CALL_TYPE_1:
        case Pyc::COMPARE_OP_FLOAT:
        case Pyc::COMPARE_OP_INT:
        case Pyc::COMPARE_OP_STR:
        case Pyc::CONTAINS_OP_DICT:
        case Pyc::CONTAINS_OP_SET:
        case Pyc::FOR_ITER_GEN:
        case Pyc::FOR_ITER_LIST:
        case Pyc::FOR_ITER_RANGE:
        case Pyc::FOR_ITER_TUPLE:
        case Pyc::LOAD_ATTR_CLASS:
        case Pyc::LOAD_ATTR_CLASS_WITH_METACLASS_CHECK:
        case Pyc::LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN:
        case Pyc::LOAD_ATTR_INSTANCE_VALUE:
        case Pyc::LOAD_ATTR_METHOD_LAZY_DICT:
        case Pyc::LOAD_ATTR_METHOD_NO_DICT:
        case Pyc::LOAD_ATTR_METHOD_WITH_VALUES:
        case Pyc::LOAD_ATTR_MODULE:
        case Pyc::LOAD_ATTR_NONDESCRIPTOR_NO_DICT:
        case Pyc::LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES:
        case Pyc::LOAD_ATTR_PROPERTY:
        case Pyc::LOAD_ATTR_SLOT:
        case Pyc::LOAD_ATTR_WITH_HINT:
        case Pyc::LOAD_CONST_IMMORTAL:
        case Pyc::LOAD_CONST_MORTAL:
        case Pyc::LOAD_GLOBAL_BUILTIN:
        case Pyc::LOAD_GLOBAL_MODULE:
        case Pyc::LOAD_SUPER_ATTR_ATTR:
        case Pyc::LOAD_SUPER_ATTR_METHOD:
        case Pyc::RESUME_CHECK:
        case Pyc::SEND_GEN:
        case Pyc::STORE_ATTR_INSTANCE_VALUE:
        case Pyc::STORE_ATTR_SLOT:
        case Pyc::STORE_ATTR_WITH_HINT:
        case Pyc::STORE_SUBSCR_DICT:
        case Pyc::STORE_SUBSCR_LIST_INT:
        case Pyc::TO_BOOL_ALWAYS_TRUE:
        case Pyc::TO_BOOL_BOOL:
        case Pyc::TO_BOOL_INT:
        case Pyc::TO_BOOL_LIST:
        case Pyc::TO_BOOL_NONE:
        case Pyc::TO_BOOL_STR:
        case Pyc::UNPACK_SEQUENCE_LIST:
        case Pyc::UNPACK_SEQUENCE_TUPLE:
        case Pyc::UNPACK_SEQUENCE_TWO_TUPLE:
        case Pyc::JUMP_BACKWARD_JIT:
        case Pyc::JUMP_BACKWARD_NO_JIT:
        case Pyc::JUMP:
        case Pyc::JUMP_NO_INTERRUPT:
        case Pyc::SETUP_CLEANUP:
        case Pyc::INSTRUMENTED_END_ASYNC_FOR_A:
        case Pyc::INSTRUMENTED_NOT_TAKEN_A:
        case Pyc::INSTRUMENTED_POP_ITER_A:
            fprintf(stderr, "Note: skipping specialized opcode %s (%d); "
                            "unexpected in marshaled bytecode\n",
                            Pyc::OpcodeName(opcode), opcode);
            break;
        default:
            fprintf(stderr, "Unsupported opcode: %s (%d)\n", Pyc::OpcodeName(opcode), opcode);
            cleanBuild = false;
            return new ASTNodeList(defblock->nodes());
        }

        else_pop =  ( (curblock->blktype() == ASTBlock::BLK_ELSE)
                      || (curblock->blktype() == ASTBlock::BLK_IF)
                      || (curblock->blktype() == ASTBlock::BLK_ELIF) )
                 && (curblock->end() == pos);
    }

    if (stack_hist.size()) {
        fputs("Warning: Stack history is not empty!\n", stderr);

        while (stack_hist.size()) {
            stack_hist.pop();
        }
    }

    if (blocks.size() > 1) {
        fputs("Warning: block stack is not empty!\n", stderr);

        while (blocks.size() > 1) {
            PycRef<ASTBlock> tmp = blocks.top();
            blocks.pop();

            blocks.top()->append(tmp.cast<ASTNode>());
        }
    }

    cleanBuild = true;
    return new ASTNodeList(defblock->nodes());
}

static void append_to_chain_store(const PycRef<ASTNode> &chainStore,
        PycRef<ASTNode> item, FastStack& stack, const PycRef<ASTBlock>& curblock)
{
    stack.pop();    // ignore identical source object.
    chainStore.cast<ASTChainStore>()->append(item);
    if (stack.top().type() == PycObject::TYPE_NULL) {
        curblock->append(chainStore);
    } else {
        stack.push(chainStore);
    }
}

static int cmp_prec(PycRef<ASTNode> parent, PycRef<ASTNode> child)
{
    /* Determine whether the parent has higher precedence than therefore
       child, so we don't flood the source code with extraneous parens.
       Else we'd have expressions like (((a + b) + c) + d) when therefore
       equivalent, a + b + c + d would suffice. */

    if (parent.type() == ASTNode::NODE_UNARY && parent.cast<ASTUnary>()->op() == ASTUnary::UN_NOT)
        return 1;   // Always parenthesize not(x)
    if (child.type() == ASTNode::NODE_BINARY) {
        PycRef<ASTBinary> binChild = child.cast<ASTBinary>();
        if (parent.type() == ASTNode::NODE_BINARY) {
            PycRef<ASTBinary> binParent = parent.cast<ASTBinary>();
            if (binParent->right() == child) {
                if (binParent->op() == ASTBinary::BIN_SUBTRACT &&
                    binChild->op() == ASTBinary::BIN_ADD)
                    return 1;
                else if (binParent->op() == ASTBinary::BIN_DIVIDE &&
                         binChild->op() == ASTBinary::BIN_MULTIPLY)
                    return 1;
            }
            return binChild->op() - binParent->op();
        }
        else if (parent.type() == ASTNode::NODE_COMPARE)
            return (binChild->op() == ASTBinary::BIN_LOG_AND ||
                    binChild->op() == ASTBinary::BIN_LOG_OR) ? 1 : -1;
        else if (parent.type() == ASTNode::NODE_UNARY)
            return (binChild->op() == ASTBinary::BIN_POWER) ? -1 : 1;
    } else if (child.type() == ASTNode::NODE_UNARY) {
        PycRef<ASTUnary> unChild = child.cast<ASTUnary>();
        if (parent.type() == ASTNode::NODE_BINARY) {
            PycRef<ASTBinary> binParent = parent.cast<ASTBinary>();
            if (binParent->op() == ASTBinary::BIN_LOG_AND ||
                binParent->op() == ASTBinary::BIN_LOG_OR)
                return -1;
            else if (unChild->op() == ASTUnary::UN_NOT)
                return 1;
            else if (binParent->op() == ASTBinary::BIN_POWER)
                return 1;
            else
                return -1;
        } else if (parent.type() == ASTNode::NODE_COMPARE) {
            return (unChild->op() == ASTUnary::UN_NOT) ? 1 : -1;
        } else if (parent.type() == ASTNode::NODE_UNARY) {
            return unChild->op() - parent.cast<ASTUnary>()->op();
        }
    } else if (child.type() == ASTNode::NODE_COMPARE) {
        PycRef<ASTCompare> cmpChild = child.cast<ASTCompare>();
        if (parent.type() == ASTNode::NODE_BINARY)
            return (parent.cast<ASTBinary>()->op() == ASTBinary::BIN_LOG_AND ||
                    parent.cast<ASTBinary>()->op() == ASTBinary::BIN_LOG_OR) ? -1 : 1;
        else if (parent.type() == ASTNode::NODE_COMPARE)
            return cmpChild->op() - parent.cast<ASTCompare>()->op();
        else if (parent.type() == ASTNode::NODE_UNARY)
            return (parent.cast<ASTUnary>()->op() == ASTUnary::UN_NOT) ? -1 : 1;
    }

    /* For normal nodes, don't parenthesize anything */
    return -1;
}

static void print_ordered(PycRef<ASTNode> parent, PycRef<ASTNode> child,
                          PycModule* mod, std::ostream& pyc_output)
{
    if (child.type() == ASTNode::NODE_BINARY ||
        child.type() == ASTNode::NODE_COMPARE) {
        if (cmp_prec(parent, child) > 0) {
            pyc_output << "(";
            print_src(child, mod, pyc_output);
            pyc_output << ")";
        } else {
            print_src(child, mod, pyc_output);
        }
    } else if (child.type() == ASTNode::NODE_UNARY) {
        if (cmp_prec(parent, child) > 0) {
            pyc_output << "(";
            print_src(child, mod, pyc_output);
            pyc_output << ")";
        } else {
            print_src(child, mod, pyc_output);
        }
    } else {
        print_src(child, mod, pyc_output);
    }
}

static void start_line(int indent, std::ostream& pyc_output)
{
    if (inLambda)
        return;
    for (int i=0; i<indent; i++)
        pyc_output << "    ";
}

static void end_line(std::ostream& pyc_output)
{
    if (inLambda)
        return;
    pyc_output << "\n";
}

int cur_indent = -1;
static void print_block(PycRef<ASTBlock> blk, PycModule* mod,
                        std::ostream& pyc_output)
{
    ASTBlock::list_t lines = blk->nodes();

    if (lines.size() == 0) {
        PycRef<ASTNode> pass = new ASTKeyword(ASTKeyword::KW_PASS);
        start_line(cur_indent, pyc_output);
        print_src(pass, mod, pyc_output);
    }

    for (auto ln = lines.cbegin(); ln != lines.cend();) {
        if ((*ln).cast<ASTNode>().type() != ASTNode::NODE_NODELIST) {
            start_line(cur_indent, pyc_output);
        }
        print_src(*ln, mod, pyc_output);
        if (++ln != lines.end()) {
            end_line(pyc_output);
        }
    }
}

void print_formatted_value(PycRef<ASTFormattedValue> formatted_value, PycModule* mod,
                           std::ostream& pyc_output)
{
    pyc_output << "{";
    print_src(formatted_value->val(), mod, pyc_output);

    switch (formatted_value->conversion() & ASTFormattedValue::CONVERSION_MASK) {
    case ASTFormattedValue::NONE:
        break;
    case ASTFormattedValue::STR:
        pyc_output << "!s";
        break;
    case ASTFormattedValue::REPR:
        pyc_output << "!r";
        break;
    case ASTFormattedValue::ASCII:
        pyc_output << "!a";
        break;
    }
    if (formatted_value->conversion() & ASTFormattedValue::HAVE_FMT_SPEC) {
        pyc_output << ":" << formatted_value->format_spec().cast<ASTObject>()->object().cast<PycString>()->value();
    }
    pyc_output << "}";
}

void print_src(PycRef<ASTNode> node, PycModule* mod, std::ostream& pyc_output)
{
    if (node == NULL) {
        pyc_output << "None";
        cleanBuild = true;
        return;
    }

    switch (node->type()) {
    case ASTNode::NODE_BINARY:
    case ASTNode::NODE_COMPARE:
        {
            PycRef<ASTBinary> bin = node.cast<ASTBinary>();
            print_ordered(node, bin->left(), mod, pyc_output);
            pyc_output << bin->op_str();
            print_ordered(node, bin->right(), mod, pyc_output);
        }
        break;
    case ASTNode::NODE_UNARY:
        {
            PycRef<ASTUnary> un = node.cast<ASTUnary>();
            pyc_output << un->op_str();
            print_ordered(node, un->operand(), mod, pyc_output);
        }
        break;
    case ASTNode::NODE_CALL:
        {
            PycRef<ASTCall> call = node.cast<ASTCall>();
            print_src(call->func(), mod, pyc_output);
            pyc_output << "(";
            bool first = true;
            for (const auto& param : call->pparams()) {
                if (!first)
                    pyc_output << ", ";
                print_src(param, mod, pyc_output);
                first = false;
            }
            for (const auto& param : call->kwparams()) {
                if (!first)
                    pyc_output << ", ";
                if (param.first.type() == ASTNode::NODE_NAME) {
                    pyc_output << param.first.cast<ASTName>()->name()->value() << " = ";
                } else {
                    PycRef<PycString> str_name = param.first.cast<ASTObject>()->object().cast<PycString>();
                    pyc_output << str_name->value() << " = ";
                }
                print_src(param.second, mod, pyc_output);
                first = false;
            }
            if (call->hasVar()) {
                if (!first)
                    pyc_output << ", ";
                pyc_output << "*";
                print_src(call->var(), mod, pyc_output);
                first = false;
            }
            if (call->hasKW()) {
                if (!first)
                    pyc_output << ", ";
                pyc_output << "**";
                print_src(call->kw(), mod, pyc_output);
                first = false;
            }
            pyc_output << ")";
        }
        break;
    case ASTNode::NODE_DELETE:
        {
            pyc_output << "del ";
            print_src(node.cast<ASTDelete>()->value(), mod, pyc_output);
        }
        break;
    case ASTNode::NODE_EXEC:
        {
            PycRef<ASTExec> exec = node.cast<ASTExec>();
            pyc_output << "exec ";
            print_src(exec->statement(), mod, pyc_output);

            if (exec->globals() != NULL) {
                pyc_output << " in ";
                print_src(exec->globals(), mod, pyc_output);

                if (exec->locals() != NULL
                        && exec->globals() != exec->locals()) {
                    pyc_output << ", ";
                    print_src(exec->locals(), mod, pyc_output);
                }
            }
        }
        break;
    case ASTNode::NODE_FORMATTEDVALUE:
        pyc_output << "f" F_STRING_QUOTE;
        print_formatted_value(node.cast<ASTFormattedValue>(), mod, pyc_output);
        pyc_output << F_STRING_QUOTE;
        break;
    case ASTNode::NODE_JOINEDSTR:
        pyc_output << (node.cast<ASTJoinedStr>()->isTemplate() ? "t" : "f") << F_STRING_QUOTE;
        for (const auto& val : node.cast<ASTJoinedStr>()->values()) {
            switch (val.type()) {
            case ASTNode::NODE_FORMATTEDVALUE:
                print_formatted_value(val.cast<ASTFormattedValue>(), mod, pyc_output);
                break;
            case ASTNode::NODE_OBJECT:
                // When printing a piece of the f-string, keep the quote style consistent.
                // This avoids problems when ''' or """ is part of the string.
                print_const(pyc_output, val.cast<ASTObject>()->object(), mod, F_STRING_QUOTE);
                break;
            default:
                /* Any other expression node is an interpolation that wasn't
                   wrapped in an ASTFormattedValue (e.g. a bare `{name}`).
                   Render it as `{expr}`. */
                pyc_output << "{";
                print_src(val, mod, pyc_output);
                pyc_output << "}";
                break;
            }
        }
        pyc_output << F_STRING_QUOTE;
        break;
    case ASTNode::NODE_KEYWORD:
        pyc_output << node.cast<ASTKeyword>()->word_str();
        break;
    case ASTNode::NODE_LIST:
        {
            pyc_output << "[";
            bool first = true;
            cur_indent++;
            for (const auto& val : node.cast<ASTList>()->values()) {
                if (first)
                    pyc_output << "\n";
                else
                    pyc_output << ",\n";
                start_line(cur_indent, pyc_output);
                print_src(val, mod, pyc_output);
                first = false;
            }
            cur_indent--;
            pyc_output << "]";
        }
        break;
    case ASTNode::NODE_SET:
        {
            pyc_output << "{";
            bool first = true;
            cur_indent++;
            for (const auto& val : node.cast<ASTSet>()->values()) {
                if (first)
                    pyc_output << "\n";
                else
                    pyc_output << ",\n";
                start_line(cur_indent, pyc_output);
                print_src(val, mod, pyc_output);
                first = false;
            }
            cur_indent--;
            pyc_output << "}";
        }
        break;
    case ASTNode::NODE_COMPREHENSION:
        {
            PycRef<ASTComprehension> comp = node.cast<ASTComprehension>();

            pyc_output << "[ ";
            print_src(comp->result(), mod, pyc_output);

            for (const auto& gen : comp->generators()) {
                pyc_output << " for ";
                print_src(gen->index(), mod, pyc_output);
                pyc_output << " in ";
                print_src(gen->iter(), mod, pyc_output);
                if (gen->condition()) {
                    pyc_output << " if ";
                    print_src(gen->condition(), mod, pyc_output);
                }
            }
            pyc_output << " ]";
        }
        break;
    case ASTNode::NODE_MAP:
        {
            pyc_output << "{";
            bool first = true;
            cur_indent++;
            for (const auto& val : node.cast<ASTMap>()->values()) {
                if (first)
                    pyc_output << "\n";
                else
                    pyc_output << ",\n";
                start_line(cur_indent, pyc_output);
                print_src(val.first, mod, pyc_output);
                pyc_output << ": ";
                print_src(val.second, mod, pyc_output);
                first = false;
            }
            cur_indent--;
            pyc_output << " }";
        }
        break;
    case ASTNode::NODE_CONST_MAP:
        {
            PycRef<ASTConstMap> const_map = node.cast<ASTConstMap>();
            PycTuple::value_t keys = const_map->keys().cast<ASTObject>()->object().cast<PycTuple>()->values();
            ASTConstMap::values_t values = const_map->values();

            auto map = new ASTMap;
            for (const auto& key : keys) {
                // Values are pushed onto the stack in reverse order.
                PycRef<ASTNode> value = values.back();
                values.pop_back();

                map->add(new ASTObject(key), value);
            }

            print_src(map, mod, pyc_output);
        }
        break;
    case ASTNode::NODE_NAME:
        pyc_output << node.cast<ASTName>()->name()->value();
        break;
    case ASTNode::NODE_NODELIST:
        {
            cur_indent++;
            for (const auto& ln : node.cast<ASTNodeList>()->nodes()) {
                if (ln.cast<ASTNode>().type() != ASTNode::NODE_NODELIST) {
                    start_line(cur_indent, pyc_output);
                }
                print_src(ln, mod, pyc_output);
                end_line(pyc_output);
            }
            cur_indent--;
        }
        break;
    case ASTNode::NODE_BLOCK:
        {
            PycRef<ASTBlock> blk = node.cast<ASTBlock>();
            if (blk->blktype() == ASTBlock::BLK_ELSE && blk->size() == 0)
                break;

            if (blk->blktype() == ASTBlock::BLK_CONTAINER) {
                end_line(pyc_output);
                print_block(blk, mod, pyc_output);
                end_line(pyc_output);
                break;
            }

            pyc_output << blk->type_str();
            if (blk->blktype() == ASTBlock::BLK_IF
                    || blk->blktype() == ASTBlock::BLK_ELIF
                    || blk->blktype() == ASTBlock::BLK_WHILE) {
                if (blk.cast<ASTCondBlock>()->negative())
                    pyc_output << " not ";
                else
                    pyc_output << " ";

                print_src(blk.cast<ASTCondBlock>()->cond(), mod, pyc_output);
            } else if (blk->blktype() == ASTBlock::BLK_FOR || blk->blktype() == ASTBlock::BLK_ASYNCFOR) {
                pyc_output << " ";
                print_src(blk.cast<ASTIterBlock>()->index(), mod, pyc_output);
                pyc_output << " in ";
                print_src(blk.cast<ASTIterBlock>()->iter(), mod, pyc_output);
            } else if (blk->blktype() == ASTBlock::BLK_EXCEPT &&
                    blk.cast<ASTCondBlock>()->cond() != NULL) {
                pyc_output << " ";
                print_src(blk.cast<ASTCondBlock>()->cond(), mod, pyc_output);
                PycRef<ASTNode> asname = blk.cast<ASTCondBlock>()->exceptAs();
                if (asname != NULL) {
                    pyc_output << " as ";
                    print_src(asname, mod, pyc_output);
                }
            } else if (blk->blktype() == ASTBlock::BLK_WITH) {
                pyc_output << " ";
                print_src(blk.cast<ASTWithBlock>()->expr(), mod, pyc_output);
                PycRef<ASTNode> var = blk.try_cast<ASTWithBlock>()->var();
                if (var != NULL) {
                    pyc_output << " as ";
                    print_src(var, mod, pyc_output);
                }
            }
            pyc_output << ":\n";

            cur_indent++;
            print_block(blk, mod, pyc_output);
            cur_indent--;
        }
        break;
    case ASTNode::NODE_OBJECT:
        {
            PycRef<PycObject> obj = node.cast<ASTObject>()->object();
            if (obj.type() == PycObject::TYPE_CODE) {
                PycRef<PycCode> code = obj.cast<PycCode>();
                decompyle(code, mod, pyc_output);
            } else {
                print_const(pyc_output, obj, mod);
            }
        }
        break;
    case ASTNode::NODE_PRINT:
        {
            pyc_output << "print ";
            bool first = true;
            if (node.cast<ASTPrint>()->stream() != nullptr) {
                pyc_output << ">>";
                print_src(node.cast<ASTPrint>()->stream(), mod, pyc_output);
                first = false;
            }

            for (const auto& val : node.cast<ASTPrint>()->values()) {
                if (!first)
                    pyc_output << ", ";
                print_src(val, mod, pyc_output);
                first = false;
            }
            if (!node.cast<ASTPrint>()->eol())
                pyc_output << ",";
        }
        break;
    case ASTNode::NODE_RAISE:
        {
            PycRef<ASTRaise> raise = node.cast<ASTRaise>();
            pyc_output << "raise ";
            bool first = true;
            for (const auto& param : raise->params()) {
                if (!first)
                    pyc_output << ", ";
                print_src(param, mod, pyc_output);
                first = false;
            }
        }
        break;
    case ASTNode::NODE_RETURN:
        {
            PycRef<ASTReturn> ret = node.cast<ASTReturn>();
            PycRef<ASTNode> value = ret->value();
            if (!inLambda) {
                switch (ret->rettype()) {
                case ASTReturn::RETURN:
                    pyc_output << "return ";
                    break;
                case ASTReturn::YIELD:
                    pyc_output << "yield ";
                    break;
                case ASTReturn::YIELD_FROM:
                    if (value.type() == ASTNode::NODE_AWAITABLE) {
                        pyc_output << "await ";
                        value = value.cast<ASTAwaitable>()->expression();
                    } else {
                        pyc_output << "yield from ";
                    }
                    break;
                }
            }
            print_src(value, mod, pyc_output);
        }
        break;
    case ASTNode::NODE_SLICE:
        {
            PycRef<ASTSlice> slice = node.cast<ASTSlice>();

            if (slice->op() & ASTSlice::SLICE1) {
                print_src(slice->left(), mod, pyc_output);
            }
            pyc_output << ":";
            if (slice->op() & ASTSlice::SLICE2) {
                print_src(slice->right(), mod, pyc_output);
            }
        }
        break;
    case ASTNode::NODE_IMPORT:
        {
            PycRef<ASTImport> import = node.cast<ASTImport>();
            if (import->stores().size()) {
                ASTImport::list_t stores = import->stores();

                pyc_output << "from ";
                if (import->name().type() == ASTNode::NODE_IMPORT)
                    print_src(import->name().cast<ASTImport>()->name(), mod, pyc_output);
                else
                    print_src(import->name(), mod, pyc_output);
                pyc_output << " import ";

                if (stores.size() == 1) {
                    auto src = stores.front()->src();
                    auto dest = stores.front()->dest();
                    print_src(src, mod, pyc_output);

                    if (src.cast<ASTName>()->name()->value() != dest.cast<ASTName>()->name()->value()) {
                        pyc_output << " as ";
                        print_src(dest, mod, pyc_output);
                    }
                } else {
                    bool first = true;
                    for (const auto& st : stores) {
                        if (!first)
                            pyc_output << ", ";
                        print_src(st->src(), mod, pyc_output);
                        first = false;

                        if (st->src().cast<ASTName>()->name()->value() != st->dest().cast<ASTName>()->name()->value()) {
                            pyc_output << " as ";
                            print_src(st->dest(), mod, pyc_output);
                        }
                    }
                }
            } else {
                pyc_output << "import ";
                print_src(import->name(), mod, pyc_output);
            }
        }
        break;
    case ASTNode::NODE_FUNCTION:
        {
            /* Actual named functions are NODE_STORE with a name */
            pyc_output << "(lambda ";
            PycRef<ASTNode> code = node.cast<ASTFunction>()->code();
            PycRef<PycCode> code_src = code.cast<ASTObject>()->object().cast<PycCode>();
            ASTFunction::defarg_t defargs = node.cast<ASTFunction>()->defargs();
            ASTFunction::defarg_t kwdefargs = node.cast<ASTFunction>()->kwdefargs();
            auto da = defargs.cbegin();
            int narg = 0;
            for (int i=0; i<code_src->argCount(); i++) {
                if (narg)
                    pyc_output << ", ";
                pyc_output << code_src->getLocal(narg++)->value();
                if ((code_src->argCount() - i) <= (int)defargs.size()) {
                    pyc_output << " = ";
                    print_src(*da++, mod, pyc_output);
                }
            }
            da = kwdefargs.cbegin();
            if (code_src->kwOnlyArgCount() != 0) {
                pyc_output << (narg == 0 ? "*" : ", *");
                for (int i = 0; i < code_src->argCount(); i++) {
                    pyc_output << ", ";
                    pyc_output << code_src->getLocal(narg++)->value();
                    if ((code_src->kwOnlyArgCount() - i) <= (int)kwdefargs.size()) {
                        pyc_output << " = ";
                        print_src(*da++, mod, pyc_output);
                    }
                }
            }
            pyc_output << ": ";

            inLambda = true;
            print_src(code, mod, pyc_output);
            inLambda = false;

            pyc_output << ")";
        }
        break;
    case ASTNode::NODE_STORE:
        {
            PycRef<ASTNode> src = node.cast<ASTStore>()->src();
            PycRef<ASTNode> dest = node.cast<ASTStore>()->dest();
            if (src.type() == ASTNode::NODE_FUNCTION) {
                PycRef<ASTNode> code = src.cast<ASTFunction>()->code();
                PycRef<PycCode> code_src = code.cast<ASTObject>()->object().cast<PycCode>();
                bool isLambda = false;

                if (strcmp(code_src->name()->value(), "<lambda>") == 0) {
                    pyc_output << "\n";
                    start_line(cur_indent, pyc_output);
                    print_src(dest, mod, pyc_output);
                    pyc_output << " = lambda ";
                    isLambda = true;
                } else {
                    pyc_output << "\n";
                    start_line(cur_indent, pyc_output);
                    if (code_src->flags() & PycCode::CO_COROUTINE)
                        pyc_output << "async ";
                    pyc_output << "def ";
                    print_src(dest, mod, pyc_output);
                    pyc_output << "(";
                }

                ASTFunction::defarg_t defargs = src.cast<ASTFunction>()->defargs();
                ASTFunction::defarg_t kwdefargs = src.cast<ASTFunction>()->kwdefargs();
                auto da = defargs.cbegin();
                int narg = 0;
                for (int i = 0; i < code_src->argCount(); ++i) {
                    if (narg)
                        pyc_output << ", ";
                    pyc_output << code_src->getLocal(narg++)->value();
                    if ((code_src->argCount() - i) <= (int)defargs.size()) {
                        pyc_output << " = ";
                        print_src(*da++, mod, pyc_output);
                    }
                }
                da = kwdefargs.cbegin();
                if (code_src->kwOnlyArgCount() != 0) {
                    pyc_output << (narg == 0 ? "*" : ", *");
                    for (int i = 0; i < code_src->kwOnlyArgCount(); ++i) {
                        pyc_output << ", ";
                        pyc_output << code_src->getLocal(narg++)->value();
                        if ((code_src->kwOnlyArgCount() - i) <= (int)kwdefargs.size()) {
                            pyc_output << " = ";
                            print_src(*da++, mod, pyc_output);
                        }
                    }
                }
                if (code_src->flags() & PycCode::CO_VARARGS) {
                    if (narg)
                        pyc_output << ", ";
                    pyc_output << "*" << code_src->getLocal(narg++)->value();
                }
                if (code_src->flags() & PycCode::CO_VARKEYWORDS) {
                    if (narg)
                        pyc_output << ", ";
                    pyc_output << "**" << code_src->getLocal(narg++)->value();
                }

                if (isLambda) {
                    pyc_output << ": ";
                } else {
                    pyc_output << "):\n";
                    printDocstringAndGlobals = true;
                }

                bool preLambda = inLambda;
                inLambda |= isLambda;

                print_src(code, mod, pyc_output);

                inLambda = preLambda;
            } else if (src.type() == ASTNode::NODE_CLASS) {
                pyc_output << "\n";
                start_line(cur_indent, pyc_output);
                pyc_output << "class ";
                print_src(dest, mod, pyc_output);
                PycRef<ASTTuple> bases = src.cast<ASTClass>()->bases().cast<ASTTuple>();
                if (bases->values().size() > 0) {
                    pyc_output << "(";
                    bool first = true;
                    for (const auto& val : bases->values()) {
                        if (!first)
                            pyc_output << ", ";
                        print_src(val, mod, pyc_output);
                        first = false;
                    }
                    pyc_output << "):\n";
                } else {
                    // Don't put parens if there are no base classes
                    pyc_output << ":\n";
                }
                printClassDocstring = true;
                PycRef<ASTNode> code = src.cast<ASTClass>()->code().cast<ASTCall>()
                                       ->func().cast<ASTFunction>()->code();
                print_src(code, mod, pyc_output);
            } else if (src.type() == ASTNode::NODE_IMPORT) {
                PycRef<ASTImport> import = src.cast<ASTImport>();
                if (import->fromlist() != NULL) {
                    PycRef<PycObject> fromlist = import->fromlist().cast<ASTObject>()->object();
                    if (fromlist != Pyc_None) {
                        pyc_output << "from ";
                        if (import->name().type() == ASTNode::NODE_IMPORT)
                            print_src(import->name().cast<ASTImport>()->name(), mod, pyc_output);
                        else
                            print_src(import->name(), mod, pyc_output);
                        pyc_output << " import ";
                        if (fromlist.type() == PycObject::TYPE_TUPLE ||
                                fromlist.type() == PycObject::TYPE_SMALL_TUPLE) {
                            bool first = true;
                            for (const auto& val : fromlist.cast<PycTuple>()->values()) {
                                if (!first)
                                    pyc_output << ", ";
                                pyc_output << val.cast<PycString>()->value();
                                first = false;
                            }
                        } else {
                            pyc_output << fromlist.cast<PycString>()->value();
                        }
                    } else {
                        pyc_output << "import ";
                        print_src(import->name(), mod, pyc_output);
                    }
                } else {
                    pyc_output << "import ";
                    PycRef<ASTNode> import_name = import->name();
                    print_src(import_name, mod, pyc_output);
                    if (!dest.cast<ASTName>()->name()->isEqual(import_name.cast<ASTName>()->name().cast<PycObject>())) {
                        pyc_output << " as ";
                        print_src(dest, mod, pyc_output);
                    }
                }
            } else if (src.type() == ASTNode::NODE_BINARY
                    && src.cast<ASTBinary>()->is_inplace()) {
                print_src(src, mod, pyc_output);
            } else {
                print_src(dest, mod, pyc_output);
                pyc_output << " = ";
                print_src(src, mod, pyc_output);
            }
        }
        break;
    case ASTNode::NODE_CHAINSTORE:
        {
            for (auto& dest : node.cast<ASTChainStore>()->nodes()) {
                print_src(dest, mod, pyc_output);
                pyc_output << " = ";
            }
            print_src(node.cast<ASTChainStore>()->src(), mod, pyc_output);
        }
        break;
    case ASTNode::NODE_SUBSCR:
        {
            print_src(node.cast<ASTSubscr>()->name(), mod, pyc_output);
            pyc_output << "[";
            print_src(node.cast<ASTSubscr>()->key(), mod, pyc_output);
            pyc_output << "]";
        }
        break;
    case ASTNode::NODE_CONVERT:
        {
            pyc_output << "`";
            print_src(node.cast<ASTConvert>()->name(), mod, pyc_output);
            pyc_output << "`";
        }
        break;
    case ASTNode::NODE_TUPLE:
        {
            PycRef<ASTTuple> tuple = node.cast<ASTTuple>();
            ASTTuple::value_t values = tuple->values();
            if (tuple->requireParens())
                pyc_output << "(";
            bool first = true;
            for (const auto& val : values) {
                if (!first)
                    pyc_output << ", ";
                print_src(val, mod, pyc_output);
                first = false;
            }
            if (values.size() == 1)
                pyc_output << ',';
            if (tuple->requireParens())
                pyc_output << ')';
        }
        break;
    case ASTNode::NODE_ANNOTATED_VAR:
        {
            PycRef<ASTAnnotatedVar> annotated_var = node.cast<ASTAnnotatedVar>();
            PycRef<ASTObject> name = annotated_var->name().cast<ASTObject>();
            PycRef<ASTNode> annotation = annotated_var->annotation();

            pyc_output << name->object().cast<PycString>()->value();
            pyc_output << ": ";
            print_src(annotation, mod, pyc_output);
        }
        break;
    case ASTNode::NODE_TERNARY:
        {
            /* parenthesis might be needed
             * 
             * when if-expr is part of numerical expression, ternary has the LOWEST precedence
             *     print(a + b if False else c)
             * output is c, not a+c (a+b is calculated first)
             * 
             * but, let's not add parenthesis - to keep the source as close to original as possible in most cases
             */
            PycRef<ASTTernary> ternary = node.cast<ASTTernary>();
            //pyc_output << "(";
            print_src(ternary->if_expr(), mod, pyc_output);
            const auto if_block = ternary->if_block().cast<ASTCondBlock>();
            pyc_output << " if ";
            if (if_block->negative())
                pyc_output << "not ";
            print_src(if_block->cond(), mod, pyc_output);
            pyc_output << " else ";
            print_src(ternary->else_expr(), mod, pyc_output);
            //pyc_output << ")";
        }
        break;
    default:
        pyc_output << "<NODE:" << node->type() << ">";
        fprintf(stderr, "Unsupported Node type: %d\n", node->type());
        cleanBuild = false;
        return;
    }

    cleanBuild = true;
}

bool print_docstring(PycRef<PycObject> obj, int indent, PycModule* mod,
                     std::ostream& pyc_output)
{
    // docstrings are translated from the bytecode __doc__ = 'string' to simply '''string'''
    auto doc = obj.try_cast<PycString>();
    if (doc != nullptr) {
        start_line(indent, pyc_output);
        doc->print(pyc_output, mod, true);
        pyc_output << "\n";
        return true;
    }
    return false;
}

void decompyle(PycRef<PycCode> code, PycModule* mod, std::ostream& pyc_output)
{
    PycRef<ASTNode> source = BuildFromCode(code, mod);

    PycRef<ASTNodeList> clean = source.cast<ASTNodeList>();
    if (cleanBuild) {
        // The Python compiler adds some stuff that we don't really care
        // about, and would add extra code for re-compilation anyway.
        // We strip these lines out here, and then add a "pass" statement
        // if the cleaned up code is empty
        if (clean->nodes().front().type() == ASTNode::NODE_STORE) {
            PycRef<ASTStore> store = clean->nodes().front().cast<ASTStore>();
            if (store->src().type() == ASTNode::NODE_NAME
                    && store->dest().type() == ASTNode::NODE_NAME) {
                PycRef<ASTName> src = store->src().cast<ASTName>();
                PycRef<ASTName> dest = store->dest().cast<ASTName>();
                if (src->name()->isEqual("__name__")
                        && dest->name()->isEqual("__module__")) {
                    // __module__ = __name__
                    // Automatically added by Python 2.2.1 and later
                    clean->removeFirst();
                }
            }
        }
        if (clean->nodes().front().type() == ASTNode::NODE_STORE) {
            PycRef<ASTStore> store = clean->nodes().front().cast<ASTStore>();
            if (store->src().type() == ASTNode::NODE_OBJECT
                    && store->dest().type() == ASTNode::NODE_NAME) {
                PycRef<ASTObject> src = store->src().cast<ASTObject>();
                PycRef<PycString> srcString = src->object().try_cast<PycString>();
                PycRef<ASTName> dest = store->dest().cast<ASTName>();
                if (dest->name()->isEqual("__qualname__")) {
                    // __qualname__ = '<Class Name>'
                    // Automatically added by Python 3.3 and later
                    clean->removeFirst();
                }
            }
        }

        // Class and module docstrings may only appear at the beginning of their source
        if (printClassDocstring && clean->nodes().front().type() == ASTNode::NODE_STORE) {
            PycRef<ASTStore> store = clean->nodes().front().cast<ASTStore>();
            if (store->dest().type() == ASTNode::NODE_NAME &&
                    store->dest().cast<ASTName>()->name()->isEqual("__doc__") &&
                    store->src().type() == ASTNode::NODE_OBJECT) {
                if (print_docstring(store->src().cast<ASTObject>()->object(),
                        cur_indent + (code->name()->isEqual("<module>") ? 0 : 1), mod, pyc_output))
                    clean->removeFirst();
            }
        }
        if (clean->nodes().back().type() == ASTNode::NODE_RETURN) {
            PycRef<ASTReturn> ret = clean->nodes().back().cast<ASTReturn>();

            PycRef<ASTObject> retObj = ret->value().try_cast<ASTObject>();
            if (ret->value() == NULL || ret->value().type() == ASTNode::NODE_LOCALS ||
                    (retObj && retObj->object().type() == PycObject::TYPE_NONE)) {
                clean->removeLast();  // Always an extraneous return statement
            }
        }
    }
    if (printClassDocstring)
        printClassDocstring = false;
    // This is outside the clean check so a source block will always
    // be compilable, even if decompylation failed.
    if (clean->nodes().size() == 0 && !code.isIdent(mod->code()))
        clean->append(new ASTKeyword(ASTKeyword::KW_PASS));

    bool part1clean = cleanBuild;

    if (printDocstringAndGlobals) {
        if (code->consts()->size())
            print_docstring(code->getConst(0), cur_indent + 1, mod, pyc_output);

        PycCode::globals_t globs = code->getGlobals();
        if (globs.size()) {
            start_line(cur_indent + 1, pyc_output);
            pyc_output << "global ";
            bool first = true;
            for (const auto& glob : globs) {
                if (!first)
                    pyc_output << ", ";
                pyc_output << glob->value();
                first = false;
            }
            pyc_output << "\n";
        }
        printDocstringAndGlobals = false;
    }

    print_src(source, mod, pyc_output);

    if (!cleanBuild || !part1clean) {
        start_line(cur_indent, pyc_output);
        pyc_output << "# WARNING: Decompyle incomplete\n";
    }
}
