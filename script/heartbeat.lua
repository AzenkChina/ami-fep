--lua script language

print("heartbeat script")

--client name
--[[
for i,v in ipairs(client)
do
	print(v)
end
--]]

--packet received from client
--[[
for i,v in ipairs(packet)
do
	print(v)
end
--]]

--confirm the heartbeat
return false
