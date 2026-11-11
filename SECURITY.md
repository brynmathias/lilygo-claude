# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in this project, **do not open a public issue**.

Report it directly to the project team lead. We will acknowledge receipt within 2 working days and aim to provide a fix or mitigation within 10 working days.

## Supported Versions

| Version | Supported |
|---|---|
| Latest release | Yes |
| Previous releases | Security fixes only |

## Security Practices

This project follows the Faculty security standards based on NCSC Secure Development & Deployment principles and MOD Secure by Design framework.

See `.rules/security.md` for the full set of security rules applied to this project.

### Key practices

- **Dependency auditing**: pip-audit, osv-scanner, and guarddog run on every merge request
- **SAST**: GitLab SAST scans all code on every merge request
- **Secrets scanning**: GitLab Secret Detection prevents accidental credential commits
- **Dependency scanning**: GitLab Dependency Scanning on every merge request
- **Container scanning**: GitLab Container Scanning on every tagged image, where the project builds one
- **Container security**: Docker Hardened Images, HEALTHCHECK, `.dockerignore`
- **SBOM**: CycloneDX bill of materials generated on every tagged release
- **Threat modelling**: STRIDE analysis in `docs/architecture/threat-model.md`
