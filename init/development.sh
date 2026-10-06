#!boot://shell.pxe
title --optional "Development"
mount --optional --read-write host
namespace create
service start text boot://textfs.pxe
session boot://session.pxe --configure-network --start-services
