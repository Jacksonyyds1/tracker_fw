# $1 is the serial port
# $2 is the credstore id
# $3 is the path to the certs
nrfcredstore $1 write $2 CLIENT_KEY $3/sim.key
nrfcredstore $1 write $2 CLIENT_CERT $3/sim.crt
nrfcredstore $1 write $2 ROOT_CA_CERT $3/sim_CA_ats.crt
nrfcredstore $1 list