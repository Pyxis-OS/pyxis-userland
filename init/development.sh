#!app://shell.pxe
title --optional "Development"
mount --optional --read-write host
session app://session.pxe --configure-network
