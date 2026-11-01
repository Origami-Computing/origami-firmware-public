"""Prepare legacy discovery metadata for exact, already-signed Public bytes."""
import publish


def payloads(data, envelope):
    public = publish.verify_manifest(envelope)
    expected = publish.manifest_payload(data, 'public', public['version'],
                                       'n16r8-rev4', 'n16r8-ota-v1')
    if public != expected:
        raise ValueError('Recovery requires the exact signed Public application')
    result = {}
    for channel in ('production', 'staging'):
        bridge = dict(public)
        bridge['channel'] = channel
        bridge['path'] = public['path'].replace('/releases/public/', '/releases/'+channel+'/', 1)
        # Legacy CDN clients require the channel and parsed staging suffix to
        # agree. This is discovery metadata; the image remains stable Public.
        if channel == 'staging':
            bridge['version'] += '-staging.1'
        result[channel] = bridge
    return result
