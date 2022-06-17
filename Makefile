LIB := snap-preload.so

.DEFAULT_GOAL := $(LIB)

%.so: %.c
	gcc -fPIC -shared -o $@ $< -ldl

install: $(LIB)
	install -t $(DESTDIR)/usr/lib -D $(LIB)
.PHONY: install

clean:
	rm -f $(LIB)
.PHONY: clean
