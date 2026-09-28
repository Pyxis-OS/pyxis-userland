#!app://shell.pxe
title --optional "Development"
mount --optional --read-write host
namespace create
service start text app://textfs.pxe
session app://session.pxe --configure-network
