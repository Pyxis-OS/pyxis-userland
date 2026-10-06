#!boot://shell.pxe
service start http boot://httpfs.pxe
service start --optional --read-only https boot://httpfs.pxe --https
session boot://shell.pxe
