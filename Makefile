# Thin wrappers so the supported path is one word. Everything real lives in
# scripts/ and BUILD.md; nothing here is required (bash scripts/check.sh works
# exactly as well).
#
#   make check       every gate, with a reason for anything skipped
#   make bootstrap   base tree + overlay + feeds + seed config
#   make build       image, then a flashable package (needs E2631_TEMPLATE)
#   make package     pack an already-collected build directory

.PHONY: help check bootstrap build package
help:
	@sed -n '/^#   make /p' Makefile

check:
	@bash scripts/check.sh

bootstrap:
	@bash scripts/bootstrap.sh

build:
	@bash scripts/build.sh

package:
	@[ -n "$${dir}" ] || { echo "usage: make package dir=<out/build-...> [template=<official.bin>]"; exit 2; }
	@node host/release.mjs --dir "$${dir}" $${template:+--template "$${template}"}
