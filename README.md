# pfsense-freebsd-patched

Branches of FreeBSD that pfSense is based on including cherry-picked patches from later releases

  * A feature branch is a branch for a specific backport
  * Patches from there are extracted and if they get integrated to pfsense-tools, the'lly receve a tag on this repo.

## pfSense and corresponding FreeBSD patches

  * pfSense 2.1.x is based on FreeBSD 8.3-RELENG
     * Feature branches are 2.1/featurename-source-of-patches
     * Once merged a tag with either merge date or last commit related to this feature
       is added like: 2.1/feature-targetbranch-YYYYMMDD
  * pfSense 2.0.x is based on FreeBSD 8.1-RELENG