#!boot://shell.pxe
service start http boot://httpfs.pxe
namespace alias json+http http
service start --optional --read-only https boot://httpfs.pxe --https
namespace alias --optional json+https https
session boot://session.pxe
