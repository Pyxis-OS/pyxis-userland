#!app://shell.pxe
mount --optional --read-only host
session app://session.pxe --configure-network
