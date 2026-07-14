SBE schema artifacts for CME connectivity. Production structs are generated/verified
ONLY from these official CME files. No stale or third-party schema files in this repo.

iLink 3 (order entry) — ilinkbinary.xml  [PRESENT]
  Schema: package=iLinkBinary, id=8, version=9, description=20251104, littleEndian.
  Version 9 is the only version CME Globex accepts (since 2026-04-26). 56 messages.
  sha256: 5e71168d3e0828925eedd9f5f1502f62ae7788b70d77fd0d5858fd45244d9a64
  Source: CME SFTP  sftpng.cmegroup.com  (login cmeconfig, public schema-distribution account)
    /MSGW/Production/Templates/ilinkbinary.xml   (also /MSGW/Cert, /MSGW/NRCert)
  Fetched 2026-07-14: Production == NRCert == Cert, byte-identical (same sha256).
  Retired v8 lives beside it on SFTP as ilinkbinary_v8.xml; not needed here.
  Refresh: Sunday prior to market open. Re-pull and re-diff before any week the schema changes.
  If environments ever diverge (NR/Cert leading Production), split into per-env copies then.

MDP 3.0 (market data) — later, public anonymous FTP:
  ftp://ftp.cmegroup.com/SBEFix/Production/Templates/templates_FixBinary.xml   (v13)

v8 -> v9 was an appended template extension (same template IDs, appended fields, larger blockLengths).
