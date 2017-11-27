BASE_DIR=$(shell pwd)
REMOTE_HOST=pi@house-pi.local
REMOTE_CONFIG_DIR=/home/homeassistant/.homeassistant

.PHONY: test
test:
	sh $(BASE_DIR)/venv/bin/activate && hass -c . --script check_config

.PHONY: remote-test
remote-test:
	ssh $(REMOTE_HOST) 'sudo -u homeassistant /bin/bash -c "source /srv/homeassistant/homeassistant_venv/bin/activate && hass --script check_config"'

.PHONY: deploy
deploy:
	$(info Deploying HA config to $(REMOTE_HOST).)
	rsync -av --exclude=venv --exclude=*.log --rsync-path="sudo -u homeassistant rsync" $(BASE_DIR)/* $(REMOTE_HOST):$(REMOTE_CONFIG_DIR)/
