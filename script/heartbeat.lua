--lua script language
--variables: client packet

print("heartbeat script")

--client name
local name = ""
for i,v in ipairs(client)
do
	name = name..string.char(v)
end

--packet received from client
local comp = ""
for i,v in ipairs(packet)
do
	comp = comp..string.char(v)
end

--confirm the heartbeat
if(name == comp)
then
	print("heartbeat packet")
	return true
else
	return false
end

