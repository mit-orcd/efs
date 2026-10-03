# Naming

[Architecture](architecture.md) · [Design rationale](design-rationale.md)

**Naming intent.** The bar for this design is that it earns the *idea* of an
"extreme filesystem": it scales as close as possible to the raw hardware.
Every design decision is checked against that bar — a component is wrong if
it serializes work the hardware could have done in parallel.

**On the name itself.** "extremfs" collides phonetically with **XtreemFS**
(an existing open-source distributed FS; the XtreemFS® trademark is
registered by Quobyte), and "EFS" is overwhelmingly associated with Amazon
Elastic File System. Neither is a good public identity for a project meant
to be found and attributed. The *idea* — never serialize what the hardware
allows in parallel — is the identity; the public **name should be chosen to
be distinctive and searchable**, and is deliberately left open here pending
a proper trademark/search check. The project uses "efs" as the working name
only.
