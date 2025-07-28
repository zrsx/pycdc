# **Decompyle++ (pycdc) — zrsx Fork**

*A blazing-fast Python bytecode decompiler & disassembler for Python 2.7–3.13*

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)](#)
[![License](https://img.shields.io/github/license/zrsx/pycdc)](https://www.gnu.org/licenses/gpl-3.0.en.html)
[![Python Versions](https://img.shields.io/badge/python-2.0--3.14-blue)](#)
[![GitHub Stars](https://img.shields.io/github/stars/zrsx/pycdc?style=social)](https://github.com/zrsx/pycdc/stargazers)
[![GitHub Forks](https://img.shields.io/github/forks/zrsx/pycdc?style=social)](https://github.com/zrsx/pycdc/network)
[![MSVC-CI](https://github.com/zrsx/pycdc/actions/workflows/msvc-ci.yml/badge.svg)](https://github.com/zrsx/pycdc/actions/workflows/msvc-ci.yml)
[![Linux-CI](https://github.com/zrsx/pycdc/actions/workflows/linux-ci.yml/badge.svg)](https://github.com/zrsx/pycdc/actions/workflows/linux-ci.yml)
[![Build Status](https://dev.azure.com/MichaelixNe/Neuro/_apis/build/status%2Fzrsx.pycdc?branchName=__main__)](https://dev.azure.com/MichaelixNe/Neuro/_build/latest?definitionId=1&branchName=__main__)
[![CircleCI Build](https://dl.circleci.com/status-badge/img/circleci/BcpbSeUMcJTaQLHYBBWuxG/UdZ4oBjzxJEuVwKKgSVwf4/tree/__main__.svg?style=shield)](https://dl.circleci.com/status-badge/redirect/circleci/BcpbSeUMcJTaQLHYBBWuxG/UdZ4oBjzxJEuVwKKgSVwf4/tree/__main__)



**Issues:**
[![Open Issues](https://img.shields.io/github/issues/zrsx/pycdc)](https://github.com/zrsx/pycdc/issues)
[![Closed Issues](https://img.shields.io/github/issues-closed/zrsx/pycdc)](https://github.com/zrsx/pycdc/issues?q=is%3Aissue+is%3Aclosed)

**Pull Requests:**
[![Open PRs](https://img.shields.io/github/issues-pr/zrsx/pycdc)](https://github.com/zrsx/pycdc/pulls)
[![Closed PRs](https://img.shields.io/github/issues-pr-closed/zrsx/pycdc)](https://github.com/zrsx/pycdc/pulls?q=is%3Apr+is%3Aclosed)

---

## **Overview**

**Decompyle++** is a fast, cross-version Python bytecode **decompiler** and **disassembler** written in C++. This fork by [**zrsx**](https://github.com/zrsx) extends the original [`pycdc`](https://github.com/zrax/pycdc) to support:

* Python **2.0** through **3.14**
* Newer opcode formats and edge cases
* Marshalled code objects and `.pyc` files
* High-speed native C++ processing

---

## **Features**

* Decompiles `.pyc` files to readable Python source
* Disassembles bytecode into opcode instructions
* Works with marshalled code objects (e.g., `.marshalled`)
* Supports Python versions **2.0 – 3.14**
* Fast, minimal, and standalone (no Python runtime needed)

---

## **Installation**

### **Dependencies**

Install the required packages:

```bash
sudo apt update
sudo apt install build-essential cmake git python3-dev
```

| Package           | Description                         |
| ----------------- | ----------------------------------- |
| `build-essential` | Compilers and build tools           |
| `cmake`           | Build system generator              |
| `git`             | Repository cloning                  |
| `python3-dev`     | Python headers (for opcode support) |

---

### **Build**

```bash
git clone https://github.com/zrsx/pycdc.git
cd pycdc
cmake .
make
```

Optional: Run the test suite

```bash
make check
```

---

## **Usage**

### Decompile `.pyc` File

```bash
./pycdc path/to/file.pyc
```

### Disassemble `.pyc` File

```bash
./pycdas path/to/file.pyc
```

### Decompile Marshalled Code

```bash
./pycdc -c -v 3.14 path/to/file.marshalled
```

#### **Flags**

| Flag | Description                                   |
| ---- | --------------------------------------------- |
| `-c` | Treat input as marshalled code                |
| `-v` | Specify Python version (e.g., `3.11`, `3.13`) |

---

## **Examples**

```bash
./pycdas __pycache__/example.cpython-312.pyc
```

---

## **Prebuilt Executables**

Looking for a ready-to-use binary?

Prebuilt `.zip` files for Windows, Linux, and macOS are automatically generated on every successful CI run.

📦 **Download from GitHub Actions Artifacts**:

| Platform     | Executable Zip                                                                 | Notes           |
|--------------|----------------------------------------------------------------------------------|-----------------|
| 🪟 Windows   | [Download](https://github.com/zrsx/pycdc/actions/workflows/msvc-ci.yml)        | Built via MSVC  |
| 🐧 Linux     | [Download](https://github.com/zrsx/pycdc/actions/workflows/linux-ci.yml)       | Built via GCC   |
| 🍎 macOS     | [Download](https://github.com/zrsx/pycdc/actions/workflows/macos-ci.yml)       | Built via Clang |

> 🔧 Visit the relevant workflow, click on the latest successful run, and scroll down to the **Artifacts** section to download.



## **Reporting Issues**

Help us improve! If you find:

* Crashes
* Incorrect output
* Unsupported opcodes (e.g., `UNSUPPORTED_OPCODE 218`)

Please open an issue at:
[https://github.com/zrsx/pycdc/issues](https://github.com/zrsx/pycdc/issues)

Include:

* Python version used to generate the `.pyc`
* The `.pyc` or `.marshalled` file (if possible)
* Full output/error logs
* Minimal source snippet (if relevant)

---

## **License**

Distributed under the terms of the
**GNU General Public License v3.0**
[View License](https://www.gnu.org/licenses/gpl-3.0.en.html)

---

## **Credits**

* **Fork Maintainer**: [zrsx](https://github.com/zrsx)
* **Original Authors**: Michael Hansen, Darryl Pogue
* **Notable Contributors**:
  charlietang98 • Kunal Parmar • Olivier Iffrig • Zlodiy • George

---

## **Contributing**

We welcome:

* Opcode/bytecode updates
* Bug reports and feature requests
* Pull requests for enhancements or fixes

**Star** the repo to support continued development.
