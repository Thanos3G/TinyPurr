# Dataset

All of the audio comes from Google Speech Commands, a free collection of one-second
recordings of single words. `build_gsc_dataset.py` turns it into an upload-ready
folder, download the archive, extract it to `gsc/`, run the script, and upload
`gsc_set/` to Edge Impulse. The three keywords are taken as they are; the fourth
class, `background`, is built from the other 32 words in the collection plus
one-second pieces of its room-noise recordings. 


```
curl -L -o speech_commands_v0.02.tar.gz http://download.tensorflow.org/data/speech_commands_v0.02.tar.gz
mkdir gsc && tar -xzf speech_commands_v0.02.tar.gz -C gsc
python dataset/build_gsc_dataset.py
edge-impulse-uploader --api-key "$EI_API_KEY" --directory gsc_set
```
