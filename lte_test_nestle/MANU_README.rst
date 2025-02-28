Provision the certs like so.
nrfcredstore /dev/tty.usbserial-AU01TP2D write 123 CLIENT_KEY sim.key
nrfcredstore /dev/tty.usbserial-AU01TP2D write 123 CLIENT_CERT sim.crt
nrfcredstore /dev/tty.usbserial-AU01TP2D write 123 ROOT_CA_CERT sim_CA_ats.crt
nrfcredstore /dev/tty.usbserial-AU01TP2D list