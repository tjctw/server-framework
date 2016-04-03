define bm
b main
r
end

define pasc
p "*async: "
p *async
p "c: "
p c
p "&(c->next): "
p &(c->next)
end

#buggy.. dont useXD
define plist
  set var $n = $arg0->next
  while $n
    printf "%X ", $n->next
    set var $n = $n->next
  end
end
