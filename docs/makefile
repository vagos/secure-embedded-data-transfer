all: build/main.pdf

build/main.pdf: main.tex
	latexmk --pdflatex='xelatex %O %S' -pdf main.tex -outdir=build --shell-escape

watch:
	fd .tex | entr -r make
