--lua script language
--variables: packet

print(os.date().."  register script")

--packet received from client
name = ""
for i,v in ipairs(packet)
do
	name = name..string.char(v)
end

print("Registered name is: "..name)

return name

