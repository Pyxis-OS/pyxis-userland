#!boot://shell.pxe
service start http bin://httpfs.pxb
namespace alias json+http http
service start --optional --read-only https bin://httpfs.pxb --https
namespace alias --optional json+https https
session boot://session.pxe
