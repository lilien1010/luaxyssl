require'bufferio'
require'socket'
require'lxyssl'

string.hex = function(x)
    local t={}
    for c in x:gmatch('(.)') do t[#t+1]=string.format("%02x", c:byte()) end
    return table.concat(t,"")
end

local function proto_index(o, k)  
    --local v = o.__proto[k]
    local v = rawget(o, '__proto')[k]
    if type(v) == "function" then return function(x,...) return v(o.__proto,...) end 
    else return v end
end

local function prototype(o)
    return setmetatable({__proto=o}, {__index = proto_index })
end

s = prototype(socket.tcp())
s:connect('192.168.0.1',80)
--x = lxyssl.event(s:getfd(),s)
--s.event = x
--s.dirty = function(...) print("dirty called"); return true end
i = 0
s:send("GET / HTTP/1.1\r\nHost: 192.168.0.1\r\n\r\n")
t1=os.time()
for i=1,1 do
--a,b=lxyssl.ev_select({s},{s},0.1)
a,b=socket.select({s},{s},0.1)
end
t2=os.time()

for k,v in pairs(b) do print("writable",i,k,v,s) end
for k,v in pairs(a) do print("readable",i,k,v,s) end
--do return 1 end

host='www.google.com'
--host='www.dreamhost.com'
--x:connect(t:getfd())
--b:settimeout(-1)
msg = string.format('GET / HTTP/1.1\r\nHost: %s\r\n\r\n', host)
for i =1,10 do
x=lxyssl.ssl()
b=bufferio.wrap(x,true)
t=socket.tcp()
t:connect(host,443)
b:connect(t:getfd())
if id then 
    lid = id
    b:sessinfo(id,master) 
end
c=0
print(msg)
repeat
    o,err,c = b:send(msg,c)
    --o,err,c = x:send('GET / HTTP/1.1\r\nhost: www.yahoo.com\r\n\r\n')
until o==#msg or err ~= "timeout"

print(b:peer(), b:name(), b:cipher())

repeat
    d,err,i = b:receive()
    --d,err,i = x:receive(1000)
    if not err then 
        print(d)
        if d=='' then 
            break 
        end
    else
    end
until err == "closed" or err=="nossl"
if err ~= "nossl" and err ~= "nossl" then b:receive('*a') end
id,master = b:sessinfo()
if id==lid then print("session reuse", id:hex(), master:hex()) end
--print(x:cipher(), x:peer(), x:name())
b:reset()
--b:close()
t:close()
end

md5=lxyssl.hash('md5')
md5:update('a')
md5:update('b')
md5:update('cde')
assert(lxyssl.hash('md5'):digest('abcde') == md5:digest())
assert(lxyssl.hash('md5'):digest('abcde') == md5:reset():digest('abcde'))
sha1=lxyssl.hash('sha1')
sha1:update('a')
sha1:update('b')
sha1:update('cde')
assert(lxyssl.hash('sha1'):digest('abcde') == sha1:digest())
assert(lxyssl.hash('sha1'):digest('abcde') == sha1:reset():digest('abcde'))
assert(lxyssl.hash('sha1'):digest() == lxyssl.hash('sha1'):digest(''))
assert(lxyssl.hash('md5'):digest() == lxyssl.hash('md5'):digest(''))
assert(lxyssl.hash('md5'):digest(''):hex() == "d41d8cd98f00b204e9800998ecf8427e")
assert(lxyssl.hash('hmac-md5','test'):digest('test'):hex()=="cd4b0dcbe0f4538b979fb73664f51abe")
assert(lxyssl.hash('hmac-sha1','test'):digest('test'):hex()=="0c94515c15e5095b8a87a50ba0df3bf38ed05fe6")

key='abcdabcdabcdabcd'
data=('a'):rep(96)
iv=lxyssl.hash('md5'):digest(key)
assert(lxyssl.aes(key):decrypt(lxyssl.aes(key):encrypt(data)) == data)
assert(lxyssl.aes(key):cbc_decrypt(lxyssl.aes(key):cbc_encrypt(data,iv),iv) == data)
assert(lxyssl.aes(key):cfb_decrypt(lxyssl.aes(key):cfb_encrypt(data,iv),iv) == data)
assert(lxyssl.rc4(key):crypt(lxyssl.rc4(key):crypt(data)) == data)
assert(#lxyssl.rand(2000) == 2000)
assert(lxyssl.rand(2000) ~= lxyssl.rand(2000))
t = {}
e = lxyssl.aes(key)
iv=lxyssl.hash('md5'):digest(key)
for i=1,10 do
    d,iv = e:cbc_encrypt(data,iv)
    t[i]=d
end

e = lxyssl.aes(key)
iv=lxyssl.hash('md5'):digest(key)
for i=1,10 do
    d,iv = e:cbc_decrypt(t[i],iv)
    t[i]=d
end

assert(table.concat(t,"")==data:rep(10))

data=('abc'):rep(95)
t = {}
e = lxyssl.aes(key)
iv=lxyssl.hash('md5'):digest(key)
start=0
for i=1,10 do
    d,iv,start = e:cfb_encrypt(data,iv, start)
    t[i]=d
end

start = 0
e = lxyssl.aes(key)
iv=lxyssl.hash('md5'):digest(key)
for i=1,10 do
    d,iv,start = e:cfb_decrypt(t[i],iv,start)
    t[i]=d
end

assert(table.concat(t,"")==data:rep(10))
