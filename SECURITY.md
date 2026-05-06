# Security Policy

## Supported Versions

The following versions of this `pycdc` fork are currently maintained and receive security updates:

| Version | Supported          |
| ------- | ------------------ |
| 5.1.x   | :white_check_mark: |
| 5.0.x   | :x:                |
| 4.0.x   | :white_check_mark: |
| < 4.0   | :x:                |

Only actively maintained branches listed above will receive patches for security vulnerabilities. Users are strongly encouraged to upgrade to a supported version.

---

## Reporting a Vulnerability

If you discover a security vulnerability in this project, report it responsibly to ensure users are protected.

### How to Report

* Open a **private security advisory** via GitHub (preferred), or
* Contact the maintainer directly if a security contact is available in the repository.

Do **not** disclose the vulnerability publicly in issues, discussions, or pull requests until it has been reviewed and addressed.

### What to Include

Provide as much detail as possible to help reproduce and assess the issue:

* A clear description of the vulnerability
* Steps to reproduce (PoC, sample bytecode, etc.)
* Affected versions or commit ranges
* Potential impact (e.g., arbitrary code execution, memory corruption, incorrect decompilation leading to unsafe assumptions)

### Response Timeline

* **Initial response:** within 3–5 days
* **Assessment & triage:** within 7–10 days
* **Fix timeline:** depends on severity and complexity

You will be notified whether the report is:

* **Accepted** → A fix will be developed and released; you may be credited unless you prefer anonymity
* **Declined** → A clear explanation will be provided

### Disclosure Policy

* Vulnerabilities will be disclosed publicly **after a fix is released**
* Coordinated disclosure is preferred to minimize risk to users

---

## Scope Notes

This project is a **Python bytecode decompiler**, and security issues may include:

* Malformed bytecode triggering crashes or undefined behavior
* Memory safety issues in the C++ core
* Incorrect decompilation that could mislead security analysis workflows

Issues purely related to **incorrect output without security implications** are treated as bugs, not vulnerabilities.
