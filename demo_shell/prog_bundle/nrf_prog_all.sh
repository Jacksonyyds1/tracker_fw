echo "Recovering and programming the network core"
nrfjprog -f NRF53 --coprocessor CP_NETWORK --program GENERATED_CP_NETWORK_merged_domains.hex --recover --verify
echo "Recovering and programming the Application core"
nrfjprog -f NRF53 --coprocessor CP_APPLICATION --program GENERATED_CP_APPLICATION_merged_domains.hex --verify
echo "Resetting the NRF5340"
nrfjprog -r
echo "Done"