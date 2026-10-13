#!boot://shell.pxe
service start http boot://httpfs.pxb
namespace alias json+http http
service start --optional --read-only https boot://httpfs.pxb --https
namespace alias --optional json+https https
session boot://session.pxe
