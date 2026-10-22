# config

`config validate` checks the installed configuration the way boot reads it:
`system://config/boot.lua`, `session.lua` and `network.lua` on the pool, as
literal data. It lists the spaces and volumes `boot.lua` defines, and checks
each space's CPUs against this machine when `system_info` is readable. Boot
applies none of the three files unless all are valid, so one invalid file means
boot starts the rescue space instead; `config validate` exits nonzero then.

Edit the files directly; they apply at the next boot. Mounted volumes,
packaged timezone data and live network availability are not checked.
