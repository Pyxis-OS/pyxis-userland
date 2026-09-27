#!app://shell.pxe
title --optional "Read-only"
mount --optional --read-only host
namespace create
session app://session.pxe
