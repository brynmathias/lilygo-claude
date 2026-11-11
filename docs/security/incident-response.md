# Incident Response Runbook

This runbook covers the response process for security incidents in lilygo-claude.

## 1. Detect

- **How was the incident discovered?** (monitoring alert, audit finding, user report, external disclosure)
- **What is the nature of the incident?** (data breach, vulnerability exploitation, credential compromise, supply chain attack)
- **When did it occur or when was it first observed?**
- **What systems, data, or users are affected?**

Record the initial details in the project's `SECURITY.md`.

## 2. Contain

- **What is the blast radius?** (single service, multiple services, external users, data exposure)
- **Immediate actions:**
  - Rotate any compromised credentials immediately
  - If a service is actively being exploited, consider taking it offline
  - Preserve logs and evidence before making changes
  - Notify the team lead and relevant stakeholders

## 3. Fix

- **Root cause analysis:** Identify how the vulnerability was introduced and why existing controls did not prevent it
- **Develop a fix:** All security fixes require peer review before merging — no exceptions
- **Test the fix:** Verify the vulnerability is resolved and no regressions are introduced
- **Deploy:** Follow normal deployment procedures; do not bypass CI gates

## 4. Review

- **Post-incident review** (within 5 working days of resolution):
  - Timeline of events
  - Root cause
  - What worked well in the response
  - What could be improved
  - Action items (with owners and deadlines)
- **Update the threat model** (`docs/architecture/threat-model.md`) with any new threats or mitigations identified
- **Update security controls** if the incident revealed a gap in existing protections
- **Share learnings** with the wider team (anonymised if necessary)

## Contacts

| Role | Contact |
|---|---|
| Team Lead | *[update]* |
| Security Lead | *[update]* |
| Infrastructure | *[update]* |
