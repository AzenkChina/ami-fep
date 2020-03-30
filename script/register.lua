--lua script language

print("register script")

--packet received from client
--[[
for i,v in ipairs(packet)
do
	print(v)
end
--]]


--return the client name
--[[
return "client"
--]]


--packet received from client
name = ""
for i,v in ipairs(packet)
do
	name = name..v
end

return name

