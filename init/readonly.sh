#!app://shell.pxe
title --optional "Read-only"
mount --optional --read-only host
session app://session.pxe
