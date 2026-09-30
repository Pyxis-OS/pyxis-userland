#!app://shell.pxe
service start http app://httpfs.pxe
service start --optional --read-only https app://httpfs.pxe --https
session app://session.pxe --remote-server 2323
