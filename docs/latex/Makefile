# FCS 技术文档 — XeLaTeX
ROOT := $(abspath .)
OUT  := $(ROOT)/build
PDF  := $(OUT)/main.pdf

.PHONY: all clean view once

all: $(PDF)

$(OUT):
	mkdir -p $(OUT)

$(PDF): main.tex chapters/*.tex | $(OUT)
	xelatex -interaction=nonstopmode -halt-on-error -output-directory=$(OUT) $(ROOT)/main.tex
	xelatex -interaction=nonstopmode -halt-on-error -output-directory=$(OUT) $(ROOT)/main.tex
	@echo "OK -> $(PDF)"

once: | $(OUT)
	xelatex -interaction=nonstopmode -halt-on-error -output-directory=$(OUT) $(ROOT)/main.tex

view: $(PDF)
	@ls -la $(PDF)

clean:
	rm -rf $(OUT)
