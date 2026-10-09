# how to build and run
1. download sophapp (git clone ssh://long.xu@10.240.0.84:29418/sophapp.git --branch=ipcam_dual)
2. copy sophapp to root of the release SDK
3. source build/envsetup_soc.sh
4. make & run
     - make ipcam_dual clean;make ipcam_dual ;make ipcam_dual install
     - ./ipcam_dual -i param_config.ini &
## Notes:
 * source build environment before build ipcam
 * most parameters can modify in *.ini files
------------------------------------------------------------------------------------
# ipcam folder tree overview
| Folder/File | Description                                             |
| ----------- | ------------------------------------------------------- |
| config      | set your build parameter                                |
| main        | app entry                                               |
| Makefile    | build app and output ipcam_cv18xx                       |
| modules     | include video audio AI OSD ...                          |
| prebuilt    | cvitek libs and 3rd libs                                |
| README.md   | notes                                                   |
| resource    | include ai models , parameter config file , and so on   |


