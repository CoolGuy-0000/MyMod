set "PROTOC=%CD%\thirdparty\protobuf-2.6.1\bin\win64\2015\staticcrt\release\protoc.exe"

"%PROTOC%" ^
  --proto_path=thirdparty\protobuf-2.6.1\src ^
  --proto_path=gcsdk ^
  --cpp_out=gcsdk ^
  gcsdk\gcsdk_gcmessages.proto
  
  
"%PROTOC%" ^
  --proto_path=thirdparty\protobuf-2.6.1\src ^
  --proto_path=gcsdk ^
  --cpp_out=gcsdk ^
  gcsdk\steammessages.proto
  
"%PROTOC%" ^
  --proto_path=thirdparty\protobuf-2.6.1\src ^
  --proto_path=gcsdk ^
  --cpp_out=gcsdk ^
  gcsdk\gcsystemmsgs.proto
  