BASE_DIR=$(shell pwd)
REMOTE_HOST=pi@house-pi.local
REMOTE_CONFIG_DIR=/home/homeassistant/.homeassistant
FLOORPLAN_DIR=$(REMOTE_CONFIG_DIR)/www/custom_ui/floorplan

.PHONY: test
test:
	sh $(BASE_DIR)/venv/bin/activate && hass -c . --script check_config

.PHONY: remote-test
remote-test:
	ssh $(REMOTE_HOST) 'sudo -u homeassistant /bin/bash -c "source /srv/homeassistant/homeassistant_venv/bin/activate && hass --script check_config"'

.PHONY: deploy
deploy:
	$(info Deploying HA config to $(REMOTE_HOST).)
	rsync -av --rsync-path="sudo -u homeassistant rsync" $(BASE_DIR)/*.yaml $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/

.PHONY: deploy-zwave
deploy-zwave:
	$(info Deploying HA config to $(REMOTE_HOST).)
	rsync -av --rsync-path="sudo -u homeassistant rsync" $(BASE_DIR)/*.xml $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/

.PHONY: pull-remote
pull-remote:
	rsync -av --include='*yaml' --include='scenes' --include='zwcfg*.xml' --exclude='*' $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/ ./

.PHONY: deploy-floorplan
deploy-floorplan:
	rsync -av --rsync-path="sudo -u homeassistant rsync" $(BASE_DIR)/ha-floorplan/floorplan.yaml $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/
	rsync -av --rsync-path="sudo -u homeassistant rsync" $(BASE_DIR)/ha-floorplan/floorplan.svg $(REMOTE_HOST):$(FLOORPLAN_DIR)/
	rsync -av --rsync-path="sudo -u homeassistant rsync" $(BASE_DIR)/ha-floorplan/floorplan.css $(REMOTE_HOST):$(FLOORPLAN_DIR)/

.PHONY: deploy-scripts
deploy-scripts:
	rsync -av --rsync-path="sudo -u homeassistant rsync" $(BASE_DIR)/python_scripts $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/
