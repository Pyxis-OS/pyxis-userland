#!boot://shell.pxe
namespace create
service start text boot://textfs.pxe
session boot://session.pxe --start-remote-services
