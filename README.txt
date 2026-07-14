Native CME connectivity built on the HFT lib (~/cpp/HFT). C++23, Linux-only, CMake.

Target: 5 microseconds p99.99 tick-to-trade (strategy currently ~2.5 mics). No vendor APIs.

- ILink3/  iLink 3 Binary Order Entry (SBE over FIXP). Schema id 8, version 9 (only version CME accepts since 2026-04-26).
- Mdp3/    (later) MDP 3.0 market data, SBE over UDP multicast via ef_vi.
- Schema/  SBE schema XMLs. Official ilinkbinary.xml comes from CME SFTP: sftpng.cmegroup.com /MSGW/{Cert,NRCert,Production}/Templates (login required).

Transport staging: bring up FIXP/SBE on Onload sockets first (correctness + cert), then swap to ef_vi CTPIO for the latency target.

Environments are config-driven: New Release (cert) and Production, one build.