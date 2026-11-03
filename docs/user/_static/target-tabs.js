function revealHashTargetTabs() {
    if (!window.location.hash) return;

    const target = document.getElementById(decodeURIComponent(window.location.hash.slice(1)));
    if (!target) return;

    const tabs = [];
    let element = target;
    while (element) {
        if (element.matches(".sd-tab-content")) tabs.push(element);
        element = element.parentElement;
    }

    for (const tab of tabs.reverse()) {
        const label = tab.previousElementSibling;
        if (label?.tagName === "LABEL" && label.htmlFor) label.click();
    }
}

window.addEventListener("hashchange", revealHashTargetTabs);
document.addEventListener("DOMContentLoaded", revealHashTargetTabs);
if (document.readyState !== "loading") revealHashTargetTabs();
