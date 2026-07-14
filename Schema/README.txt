CME SBE schemas + code generation. Production structs are generated/verified ONLY from
these official CME files. No stale or third-party schema files in this repo.

Regenerate everything (downloads latest schemas, then regenerates both headers):
  ./regen.sh [Production|NRCert|Cert]     (default Production)

Files:
  ilinkbinary.xml           iLink 3 schema (package iLinkBinary, id 8, version 9).
                            Source: CME SFTP sftpng.cmegroup.com (public cmeconfig login)
                            /MSGW/<env>/Templates/ilinkbinary.xml.
                            v9 is the only version CME Globex accepts (since 2026-04-26).
  templates_FixBinary.xml   MDP 3.0 schema (package mktdata, id 1, version 13).
                            Source: anonymous FTP ftp.cmegroup.com
                            /SBEFix/<env>/Templates/templates_FixBinary.xml.
  gen_sbe.py                Generator: SBE XML -> #pragma pack(1) cast-over-the-wire structs
                            with glaze ToString(), HFT house style. Profiles: ilink3, mdp3.
  ILink3Sbe.hpp             GENERATED, namespace ILink3 (56 messages). Do not edit.
  Mdp3Sbe.hpp               GENERATED, namespace Mdp3 (31 messages). Do not edit.
  CMakeLists.txt            SchemaLib (compiles both headers every build -> all
                            static_assert(sizeof == BlockLength) layout checks run)
                            + SchemaTests (NewOrderSingle print demo).

CME refreshes schema files on the Sunday prior to market open; regen.sh reports
"SCHEMA CHANGED version X -> Y" when a pull differs, and generation is deterministic,
so the git diff of the generated headers shows exactly what CME changed.

PHASE 2 (pending): repeating <group> cursors + var-length <data>. iLink3's order hot path
is flat root blocks (covered); MDP3 incremental refresh messages are group-heavy, so
group support is required before the MDP3 feed handler can decode book entries.
