Native CME connectivity built on the HFT lib (~/cpp/HFT). C++23, Linux-only, CMake.

Target: 5 microseconds p99.99 tick-to-trade (strategy currently ~2.5 mics). No vendor APIs.

- ILink3/  iLink 3 Binary Order Entry (SBE over FIXP). Schema id 8, version 9 (only version CME accepts since 2026-04-26).
- Mdp3/    (later) MDP 3.0 market data, SBE over UDP multicast via ef_vi.
- Schema/  SBE schema XMLs. Official ilinkbinary.xml comes from CME SFTP: sftpng.cmegroup.com /MSGW/{Cert,NRCert,Production}/Templates (login required).

Transport staging: bring up FIXP/SBE on Onload sockets first (correctness + cert), then swap to ef_vi CTPIO for the latency target.

Environments are config-driven: New Release (cert) and Production, one build.

Version control:
- Private repo: https://github.com/emilbigaj/CME  (branch: master)
- Clone: git clone https://github.com/emilbigaj/CME.git
- Auth: GitHub CLI (gh) is git's HTTPS credential helper for account emilbigaj, so push/pull just work (no token in-repo).
- Secrets: Config/*/ILink3.json is gitignored (real CME credentials). The Cert + New Release configs were force-added into this PRIVATE repo, so their live keys are in git history. Production configs stay ignored by default — never commit them in plaintext; force-add deliberately or encrypt (git-crypt/SOPS) first.