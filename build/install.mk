ifeq ("$(STORAGE_TYPE)", "")
	FLASH_PARTITION_XML := $(BUILD_PATH)/boards/default/partition/partition_none.xml
else

ifeq ($(CONFIG_SKIP_UBOOT),y)
ifeq ($(CONFIG_MEDIA_SUPPORT_PROC),y)
	FLASH_PARTITION_XML := $(BUILD_PATH)/boards/$(shell echo $(CHIP_ARCH) | tr 'A-Z' 'a-z')$(BUILD_CHIP_ARCH)/$(PROJECT_FULLNAME)/partition/partition_fastboot_debug_$(STORAGE_TYPE).xml
else
	FLASH_PARTITION_XML := $(BUILD_PATH)/boards/$(shell echo $(CHIP_ARCH) | tr 'A-Z' 'a-z')$(BUILD_CHIP_ARCH)/$(PROJECT_FULLNAME)/partition/partition_fastboot_$(STORAGE_TYPE).xml
endif
else
	FLASH_PARTITION_XML := $(BUILD_PATH)/boards/$(shell echo $(CHIP_ARCH) | tr 'A-Z' 'a-z')$(BUILD_CHIP_ARCH)/$(PROJECT_FULLNAME)/partition/partition_$(STORAGE_TYPE).xml
endif
endif

define pack_param
	@dd if=${1} of=$(APP_INSTALL_DIR)/rawimages/param.bin conv=sync ibs=61440
	@python3 $(COMMON_TOOLS_PATH)/image_tool/packpq.py -i $(APP_INSTALL_DIR)/rawimages/param.bin -o $(OUTPUT_DIR)/rawimages/param.bin
	@python3 $(COMMON_TOOLS_PATH)/image_tool/raw2cimg.py $(OUTPUT_DIR)/rawimages/param.bin $(OUTPUT_DIR) $(FLASH_PARTITION_XML)
	@if [ -e $(OUTPUT_DIR)/param.bin ];then cp -rf $(OUTPUT_DIR)/param.bin $(APP_INSTALL_DIR)/; fi
endef

install:
	@mkdir -p $(APP_INSTALL_DIR)
	@mkdir -p $(APP_INSTALL_DIR)/rawimages
	@cp -rf $(TARGET) $(APP_INSTALL_DIR)
ifeq ($(CONFIG_MODULE_PREVIEW),y)
	@install -m 755 $(TARGET_OUT_DIR)/bin/bf01_atbm_ap $(APP_INSTALL_DIR)/bf01_atbm_ap
	@install -m 755 $(APP_RESOURCE_DIR)/preview/bf01-preview $(APP_INSTALL_DIR)/bf01-preview
	@install -m 755 $(APP_RESOURCE_DIR)/preview/auto.sh.example $(APP_INSTALL_DIR)/auto.sh.example
	@install -m 755 $(APP_RESOURCE_DIR)/preview/auto.sh.example $(APP_INSTALL_DIR)/auto.sh
	@install -m 755 $(APP_RESOURCE_DIR)/preview/S99zpreview $(APP_INSTALL_DIR)/S99zpreview
	@install -m 755 $(SRCTREE)/scripts/install-preview.sh $(APP_INSTALL_DIR)/install-preview.sh
endif
#BITMAP
ifeq ($(CONFIG_RESOURCE_INSTALL_BITMAP), y)
	@cp -f $(APP_RESOURCE_DIR)/bitmap/tiger.bmp $(APP_INSTALL_DIR)
endif
#VDEC
ifeq ($(CONFIG_RESOURCE_INSTALL_VDEC_FILES), y)
	@cp -rpf $(APP_RESOURCE_DIR)/vdec/* $(APP_INSTALL_DIR)
endif
#AI
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV180X_MOBILEDETV2_PEDESTRIAN_D0_LS_384),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv180x/mobiledetv2-pedestrian-d0-ls-384.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV180X_MOBILEDETV2_PEDESTRIAN_D0_LS_448),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv180x/mobiledetv2-pedestrian-d0-ls-448.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_CVIFACE_V5),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/cviface-v5-s.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_HAND_DET_QAT_640X360),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/hand_det_qat_640x384.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_HAND_KPT_128X128),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/hand_kpt_128x128.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_HAND_KPT_CLS9),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/hand_kpt_cls9.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_HARDHAT_720_1280),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/hardhat_720_1280.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_IR_RECOGITION),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/ir_recogition.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_LIVENESS),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/liveness.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_MOBILEDETV2_PEDESTRIAN_D0_LS_384),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/mobiledetv2-pedestrian-d0-ls-384.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_MOBILEDETV2_PEDESTRIAN_D0_LS_448),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/mobiledetv2-pedestrian-d0-ls-448.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_MOBILEDETV2_PEDESTRIAN_D1_LS),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/mobiledetv2-pedestrian-d1-ls.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_PIPNET_MBV3_64_V5_2),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/pipnet_mbv3_64_v5_cv181x_2.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_RETINAFACE_MASK),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/retinaface_mask.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_SCRFD_320X256),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/scrfd_320_256.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_SCRFD_500M_BNKPS_432X768),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/scrfd_500m_bnkps_432_768.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_SOUND_IPC_SR8K_V14),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/sound_ipc_sr8k_v14_cv181x.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_SOUND_IPC_SR8K_V17),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/sound_ipc_sr8k_v17_cv181x.cvimodel $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_AI_CV181X_YOLOV8N_HEADPERSON_KPT),y)
	@cp -f $(APP_RESOURCE_DIR)/ai_models/cv181x/yolov8n_headperson_kpt_cv181x.cvimodel $(APP_INSTALL_DIR)
endif
#FILE  RECOVRY
ifeq ($(CONFIG_RESOURCE_INSTALL_H264_PCM_TEMPLATE),y)
	@cp -f $(APP_RESOURCE_DIR)/file_recover/h264_pcm_template.bin $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_H265_PCM_TEMPLATE),y)
	@cp -f $(APP_RESOURCE_DIR)/file_recover/h265_pcm_template.bin $(APP_INSTALL_DIR)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_MP4_AAC_TEMPLATE),y)
	@cp -f $(APP_RESOURCE_DIR)/file_recover/mp4_aac_template.bin $(APP_INSTALL_DIR)
endif
#NETWORK
ifeq ($(CONFIG_RESOURCE_INSTALL_NETWORK_WWW),y)
	@cp -rf $(APP_RESOURCE_DIR)/www $(APP_INSTALL_DIR)
endif
#PARAM
ifeq ($(CONFIG_RESOURCE_INSTALL_FBM_CV180ZB_WEVB_GC2053_AI),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/fbm/cv180zb_wevb_gc2053_ai.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/fbm/cv180zb_wevb_gc2053_ai.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_FBM_CV1811C_CV2003_CV2003_CV2003),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/fbm/cv1811c_wevb_cv2003_cv2003_cv2003.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/fbm/cv1811c_wevb_cv2003_cv2003_cv2003.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_FBM_CV1811C_WEVB_GC4653),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/fbm/cv1811c_wevb_gc4653.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/fbm/cv1811c_wevb_gc4653.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_FBM_CV1811H_WEVB_GC4653_UVC),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/fbm/cv1811h_wevb_gc4653_uvc.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/fbm/cv1811h_wevb_gc4653_uvc.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_FBM_CV1811H_WEVB_GC2093_GC2053_AUTOTEST),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/fbm/cv1811h_wevb_gc2093_gc2053_autotest.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/fbm/cv1811h_wevb_gc2093_gc2053_autotest.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_FBM_CV180ZB_WEVB_SC2336_AUTOTEST),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/fbm/cv180zb_wevb_sc2336_autotest.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/fbm/cv180zb_wevb_sc2336_autotest.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_SBM_CV1812HA_WEVB_GC2053),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/sbm/cv1812ha_wevb_gc2053.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/sbm/cv1812ha_wevb_gc2053.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_SBM_CV1811H_WEVB_GC4653_GC4653),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/sbm/cv1811h_wevb_gc4653_gc4653.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/sbm/cv1811h_wevb_gc4653_gc4653.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_SBM_CV180ZB_WEVB_GC2053),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/sbm/cv180zb_wevb_gc2053.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/sbm/cv180zb_wevb_gc2053.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_SBM_CV180ZB_WEVB_SC2331_SC2331),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/sbm/cv180zb_wevb_sc2331_sc2331.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/sbm/cv180zb_wevb_sc2331_sc2331.ini)
endif
ifeq ($(CONFIG_RESOURCE_INSTALL_SBM_CV180ZB_WEVB_CV2003_CV2003),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/sbm/cv180zb_wevb_cv2003_cv2003.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/sbm/cv180zb_wevb_cv2003_cv2003.ini)
endif

ifeq ($(CONFIG_RESOURCE_INSTALL_FBM_CV1815JA_BF01_GC4683),y)
	@cp -rf $(APP_RESOURCE_DIR)/parameter/fbm/cv1815ja_bf01_gc4683.ini $(APP_INSTALL_DIR)/param_config.ini
	@$(call pack_param ,$(APP_RESOURCE_DIR)/parameter/fbm/cv1815ja_bf01_gc4683.ini)
endif

pack_data: install
	@bash $(SRCTREE)/build_cvitek.sh pack_data cv1815ja_bf01_spinand

.PHONY: install pack_data
