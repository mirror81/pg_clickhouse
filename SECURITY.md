# pg_clickhouse Security Vulnerability Response Policy

## Security Change Log and Support

Details regarding security fixes are publicly reported in the [change log].

## Reporting a Vulnerability

We're extremely grateful for security researchers and users who report
vulnerabilities to the pg_clickhouse Open Source Community. Developers
thoroughly investigate all reports.

To report a potential vulnerability in pg_clickhouse please send the details
about it through our public bug bounty program hosted by [Bugcrowd] and be
rewarded for it as per the program scope and rules of engagement.

### When Should I Report a Vulnerability?

*   You think you discovered a potential security vulnerability in pg_clickhouse
*   You are unsure how a vulnerability affects pg_clickhouse

### When Should I **Not** Report a Vulnerability?

*   You need help tuning pg_clickhouse components for security
*   You need help applying security related updates
*   Your issue is not related to security

## Security Vulnerability Response

pg_clickhouse maintainers acknowledged and analyze each report within 5
working days. As a security issue moves from triage to identified fix to
release planning, we will keep the reporter updated.

## Public Disclosure Timing

The pg_clickhouse maintainers and the bug submitter will negotiate a public
disclosure date. We prefer to fully disclose the bug as soon as possible once
a user mitigation is available. It is reasonable to delay disclosure when the
bug or the fix is not yet fully understood, the solution is not well-tested,
or for vendor coordination. The disclosure timeframe ranges from immediate
(especially if it's already publicly known) to 90 days. For a vulnerability
with a straightforward mitigation, we expect the report date to disclosure
date to be on the order of 7 days.

## Embargo Policy

Open source users and support customers may subscribe to receive alerts during
the embargo period by visiting the [Trust Center], requesting access, and
subscribing for alerts. Subscribers agree not to make these notifications
public, issue communications, share this information with others, or issue
public patches before the disclosure date. Accidental disclosures must be
reported immediately to trust@clickhouse.com. Failure to follow this policy or
repeated leaks may result in removal from the subscriber list.

### Participation criteria:

1.  Be a current open source user or support customer with a valid corporate
    email domain (no @gmail.com, @azure.com, etc.).
2.  Sign up to the pg_clickhouse [Trust Center]
3.  Accept the pg_clickhouse Security Vulnerability Response Policy as
    outlined here.
4.  Subscribe to pg_clickhouse OSS Trust Center alerts.

### Removal criteria:

1.  Members may be removed for failure to follow this policy or for repeated
    leaks.
2.  Members may be removed for bounced messages (mail delivery failure).
3.  Members may unsubscribe at any time.

### Notification process:

pg_clickhouse will post notifications within the [Trust Center] and notify
subscribers. Subscribers must log in to the Trust Center to download the
notification. The notification will include the timeframe for public
disclosure.

  [change log]: CHANGELOG.md
  [Bugcrowd]: https://bugcrowd.com/clickhouse
  [Trust Center]: https://trust.clickhouse.com/?product=pg_clickhouse
