#!app://shell.pxe
title --optional "Development"
mount --optional --read-write host
namespace create
session app://session.pxe --configure-network
