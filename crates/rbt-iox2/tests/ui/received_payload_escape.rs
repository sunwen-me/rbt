use rbt_iox2::{Image, SliceSubscriber};

fn leaked_payload(subscriber: &SliceSubscriber<Image>) -> &[u8] {
    let payload = {
        let sample = subscriber.receive().unwrap().unwrap();
        sample.payload()
    };
    payload
}

fn main() {
    let _ = leaked_payload;
}
