default:
	@echo "Please run 'make' under any subprojects to compile."

COMPILE_COMMANDS_PROFILE ?= all

compile-commands:
	@./scripts/update_compile_commands.py baseline --profile $(COMPILE_COMMANDS_PROFILE) --keep-going

compile-commands-list:
	@./scripts/update_compile_commands.py baseline --profile $(COMPILE_COMMANDS_PROFILE) --list

compile-commands-merge:
	@./scripts/update_compile_commands.py merge

.PHONY: default compile-commands compile-commands-list compile-commands-merge
