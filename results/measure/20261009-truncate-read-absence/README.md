# Authoritative absence after peer truncate

The retained staging map is not proof that a published chunk still exists.
Published-image retry now consumes the successful GETCHUNKS absence bitmap,
returns an observed hole and clears its publication observation without
removing locally owned staging data. Failed authority reads still fail.
The deterministic regression fails on the original code and passes here.

AMD private direct-I/O/TCP, pressure enabled only in fixture:
- Full Linux build and regression PASS.
- Eight peer append/truncate races, cold restart, concurrent append and
  physical reclamation PASS (gc-direct-ki5qar2t).
- Combined gate gc-direct-ks9uhap3: core POSIX 212/212; peer POSIX 64/64;
  exact session lease/lock cleanup across crash and physical reclamation PASS.
- Full GC gate including failed-delete retry, offline member rejoin, cold
  inventory and quota zero, late PUT fences PASS.

Installed 978caaf7 restored with pressure off and both mounts serving.
D25, production session integration and pressure activation remain staged.
This is a single-host four-daemon check, not hardware power-loss proof.
