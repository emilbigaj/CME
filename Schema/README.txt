SBE schema artifacts for CME connectivity.

We build production structs ONLY from CME's official current schema. No stale or
third-party schema files live in this repo.

iLink 3 (order entry) — schema id 8, version 9:
  Official ilinkbinary.xml (NOT public) via CME SFTP, login required:
    sftpng.cmegroup.com : /MSGW/Cert/Templates, /MSGW/NRCert/Templates, /MSGW/Production/Templates
  Version 9 is the only version CME Globex accepts (since 2026-04-26).
  Update schedule: Sunday prior to market open.

MDP 3.0 (market data) — public, anonymous FTP:
  ftp://ftp.cmegroup.com/SBEFix/Production/Templates/templates_FixBinary.xml   (v13)

Drop the official v9 ilinkbinary.xml in this folder when it arrives; structs are
generated/verified against it. v8 -> v9 is an appended template extension (same
template IDs, appended fields, larger blockLengths).
