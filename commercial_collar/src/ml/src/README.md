Creating the CBOR encode/decode function

To work with the version of NCS we are using, the zcbor CDDL compiler must be rolled back to version 0.7.0.

To do this, create a new Python virtual environment ... let's call it test-env-cbor:
```
python -m venv test-env-cbor
```
Activate this virtual environment, and install zcbor at v0.7.0:
```
. test-env-cbor/bin/activate
pip3 install zcbor==0.7.0
```
Now generate the c and headers:
```
zcbor code -c inference.cddl -d --oc ml_decode.c --oh ml_decode.h  -t Inference
zcbor code -c inference.cddl -e --oc ml_encode.c --oh ml_encode.h  -t Inference
```

To simplify things for those _not_ using CDDL, the two generated files `ml_encode_type.h`
and `ml_decode_types.h` are essentially identical. They are replaced by `ml_types.h`
