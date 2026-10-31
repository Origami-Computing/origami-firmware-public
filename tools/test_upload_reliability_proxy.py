"""The real staging fault-proxy gate must never forward unrelated device media."""
from upload_reliability_device_check import is_fixture_upload

def multipart(filename='test-fixed.jpg', payload=b'fixture', fields=None, extra=b''):
    if fields is None: fields=[('platform',b'device'),('sessionId',b'origami-device')]
    data=b'--gate\r\nContent-Disposition: form-data; name="media"; filename="'+filename.encode()+b'"\r\nContent-Type: image/jpeg\r\n\r\n'+payload+b'\r\n'
    for name,value in fields:data+=b'--gate\r\nContent-Disposition: form-data; name="'+name.encode()+b'"\r\n\r\n'+value+b'\r\n'
    return data+extra+b'--gate--\r\n'
fixtures={'test-fixed.jpg':b'fixture'}
def allowed(body):return is_fixture_upload(body,'multipart/form-data; boundary=gate',fixtures)
assert allowed(multipart())
assert allowed(multipart(fields=[('platform',b'device'),('sessionId',b'origami-device'),('agentMode',b'timeline_only')]))
assert not allowed(multipart(filename='personal.jpg'))
assert not allowed(multipart(payload=b'personal bytes'))
assert not allowed(multipart(fields=[('platform',b'web'),('sessionId',b'origami-device')]))
assert not allowed(multipart(fields=[('platform',b'device'),('platform',b'device'),('sessionId',b'origami-device')]))
assert not allowed(multipart(fields=[('platform',b'device'),('sessionId',b'origami-device'),('text',b'send a message')]))
assert not allowed(multipart(extra=b'--gate\r\nContent-Disposition: form-data; name="media"; filename="test-fixed.jpg"\r\n\r\nfixture\r\n'))
assert not allowed(b'not multipart')
assert not allowed(b'private preamble\r\n'+multipart())
assert not allowed(multipart()+b'private epilogue')
assert not allowed(multipart().replace(b'--gate--\r\n',b''))
print('PASS actual staging proxy gate: exact generated file only, constrained intent, no duplicate fields or personal payloads')
