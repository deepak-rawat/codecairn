// café 🧭
interface Props { label: string; }
export function shared(props: Props) {
    const local = props.label;
    return <section><span>{local}</span></section>;
}
