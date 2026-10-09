# Admission arithmetic regression

AMD private source tree, Linux build and full regression exit 0.
Compressed logs retain the build and regression output. The final additional
host-pressure boundary tests were run separately and passed on AMD and macOS.

Tests exercise saturated body/entry multiplication, combined charge addition,
and cross-group count/body/log aggregation, with existing recovery reserve,
follower pacing, leader eligibility and persistence poison checks retained.
No installed-service change or pressure/D25 activation in this checkpoint.
