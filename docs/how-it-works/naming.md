# Naming

[Architecture](architecture.md) · [Design rationale](design-rationale.md)

**Naming intent.** The bar for this design is that it earns the *idea* of an
"extreme filesystem": it scales as close as possible to the raw hardware.
Every design decision is checked against that bar — a component is wrong if
it serializes work the hardware could have done in parallel.

**On the name itself.** "extremfs" is close in spelling and sound to the
existing [XtreemFS project](https://www.xtreemfs.org/), whose site attributes
its trademark registration to Quobyte. "EFS" is also the name used for
[Amazon Elastic File System](https://docs.aws.amazon.com/efs/latest/ug/).
These primary-source identity checks were made Oct 7, 2026; they do not
establish trademark clearance or quantify search-engine dominance.

A distinctive, searchable public name remains a design goal, deliberately
open pending a proper name/search check. This project continues to use "efs"
as its working name; this review does not choose or approve a replacement.
Earlier wording is [preserved](../archive/naming-wording-before-round5-20261007.md).
