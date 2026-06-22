# Security Policy

## Supported Versions

| Version | Supported |
|---|---|
| Unreleased (pre-1.0) | Yes — all reported issues addressed |

Once 1.0.0 is released this table will list supported minor branches.

## Reporting a Vulnerability

**Do not open a public GitHub issue for security vulnerabilities.**

Email **security@fighterslegacy.org** with:

- A description of the vulnerability
- Steps to reproduce
- Potential impact
- Any suggested mitigations

### Response Timeline

| Step | Target |
|---|---|
| Acknowledgement | Within 48 hours |
| Initial assessment | Within 7 days |
| Fix timeline communicated | After assessment |

## Disclosure Policy

Vulnerabilities are fixed in a private branch. A coordinated public disclosure follows once a fix is ready. Reporters are credited in the release notes unless they request anonymity. CVE coordination is handled if warranted.

## Scope

**In scope:**
- Remote code execution
- Privilege escalation
- Data leakage from the engine or content system
- Buffer overflows in asset/network parsers

**Out of scope:**
- Typos or documentation errors
- Issues requiring physical access to the machine
- Vulnerabilities in third-party dependencies — report those upstream
