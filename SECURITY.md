# Security Policy

TextDeck includes a Windows kernel-mode virtual HID driver. Treat suspected driver,
input-injection, privilege-boundary, and installer vulnerabilities as security-sensitive.

## Supported releases

There is currently no production-signed driver release. The repository's documented
test-signed driver is for development and controlled testing only; do not install it
on a machine you rely on.

## Reporting a vulnerability

Please do not publish exploit details, weaponized proof-of-concept code, or sensitive
logs in a public issue.

1. If GitHub's **Security → Report a vulnerability** option is available, use it.
2. If private vulnerability reporting is unavailable, open a public issue asking the
   maintainer to arrange a private disclosure channel; do not include technical details
   until a private channel is established.

Include the affected commit or version, Windows build, impact, and minimal reproduction
steps once a private channel is available. Remove credentials, private keys, personal
paths, and unrelated machine identifiers from logs.

There is no guaranteed response-time SLA.