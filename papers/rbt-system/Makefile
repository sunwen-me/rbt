LATEXMK ?= latexmk
ZIP ?= zip

PDF_OUTPUT := ../../output/pdf/rbt-system-zh.pdf
SOURCE_ZIP := ../../output/pdf/rbt-system-latex.zip
SOURCE_FILES := main.tex results.tex README.md Makefile evidence.json

.PHONY: all pdf sourcezip clean

all: pdf

pdf: build/main.pdf
	mkdir -p "$(dir $(PDF_OUTPUT))"
	cp "$<" "$(PDF_OUTPUT)"

build/main.pdf: main.tex results.tex
	mkdir -p build
	$(LATEXMK) -xelatex -interaction=nonstopmode -halt-on-error -outdir=build main.tex

sourcezip:
	mkdir -p "$(dir $(SOURCE_ZIP))"
	rm -f "$(SOURCE_ZIP)"
	$(ZIP) -j -q "$(SOURCE_ZIP)" $(SOURCE_FILES)

clean:
	rm -rf build
