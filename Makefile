BASE_DIR="$(shell pwd)"
REMOTE_HOST=hassio@10.0.0.2
REMOTE_CONFIG_DIR=/config

.PHONY: test
test:
	sh $(BASE_DIR)/venv/bin/activate && hass -c . --script check_config

.PHONY: remote-test
remote-test:
	ssh $(REMOTE_HOST) 'sudo -u homeassistant /bin/bash -c "source /srv/homeassistant/homeassistant_venv/bin/activate && hass --script check_config"'

.PHONY: deploy
deploy:
	$(info Deploying HA config to $(REMOTE_HOST).)
	rsync -av --rsync-path="sudo rsync" $(BASE_DIR)/*.yaml $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/
	rsync -av --rsync-path="sudo rsync" $(BASE_DIR)/scripts/*.yaml $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/scripts/
	rsync -av --rsync-path="sudo rsync" $(BASE_DIR)/esphome/*.yaml $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/esphome/
	rsync -av --rsync-path="sudo rsync" $(BASE_DIR)/esphome/common/*.yaml $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/esphome/common/
	rsync -av --rsync-path="sudo rsync" $(BASE_DIR)/panels/*.html $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/panels/

.PHONY: deploy-zwave
deploy-zwave:
	$(info Deploying HA config to $(REMOTE_HOST).)
	rsync -av --rsync-path="sudo rsync" $(BASE_DIR)/*.xml $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/

.PHONY: pull-remote
pull-remote:
	rsync -av --include='*yaml' --include='esphome' --include='scripts' --include='panels' --include='zwavegraph2.html' --include='scenes' --include='zwcfg*.xml' --exclude='*' $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/ ./

.PHONY: deploy-scripts
deploy-scripts:
	rsync -av --delete --rsync-path="sudo rsync" $(BASE_DIR)/python_scripts $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/

.PHONY: deploy-custom-components
deploy-custom-components:
	rsync -av --rsync-path="sudo rsync" $(BASE_DIR)/custom_components $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/
