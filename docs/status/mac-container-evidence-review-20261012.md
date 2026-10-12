# Mac container evidence review — October 12

Reviewed commit `4764dada1302a17e05b21996f63413e3964d75d6` on mac-container-8b7391b3. Evidence records exact implementation8b7391b3, containers/direct/TCP on three private Mac-hosted nodes,756 source/test inputs unchanged, payload O_DIRECT observed, focused storage gates and container/store translation-unit ASan/UBSan PASS. Shared library/writer objects were not sanitizer-instrumented.

Live run records256 operations per phase,128MiB create/read payload (512KiB per file),256 overwrites totaling32MiB, warm/read and restart exact integrity verification, deletion and zero physical mappings/container allocation on all three nodes. All phase errors0; create1.90MiB/s, read11.14MiB/s, cold verify10.11MiB/s, delete24.49ops/s. Proof states installed service unchanged.

Boundaries: one repeat, older source8b7391b3, containers engine and bounded workload; no current03bd files-engine/seven-client qualification, hardware power-loss proof or full-stack sanitizer claim. Prose says two writers but phase results report four workload workers; clarify terminology before comparing throughput. This is a report/evidence discrepancy, not proof the recorded integrity assertions failed. Native multi-root/crash/corruption claims remain bounded as documented.

Disposition: retain as useful deferred-container evidence, do not import the branch's implementation or transfer passes into this alpha. Containers remain excluded. The commit contains evidence/documentation only. It does not resolve ordinary-I/O errors, deep-directory deadlines or establish `.efs`-disabled acceptance.
