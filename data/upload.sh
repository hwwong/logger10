#!/bin/bash



echo "compress index.htm"
gzip -c -9 index.org > index.htm.gz


echo "Uploading compressed index.htm to ESP8266"
curl -F "file=@index.htm.gz" tc10.local/edit




echo "Done"
