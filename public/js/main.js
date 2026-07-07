document.addEventListener('DOMContentLoaded', () => {
    const button = document.getElementById('button');
    const message = document.getElementById('message');
    const navigateButton = document.getElementById('hidden-button');

    navigateButton.hidden = true;

    const responses = {
        1: "ok lets go click somewhere",
        4: "ok i mean click here",
        5: "do u not see it???",
        9: "ok fine click here"
    };

    let clickCount = 0;
    let typeWriter = null;

    button.addEventListener('click', () => {
        clickCount++;

        const response = responses[clickCount];
        if (!response) return;

        if (typeWriter) clearInterval(typeWriter);

        if (message.textContent.length > 0) message.textContent += "\n";

        let i = 0;
        typeWriter = setInterval(() => {
            if (i < response.length) {
                message.textContent += response.charAt(i);
                i++;
            } else {
                clearInterval(typeWriter);
            }
        }, 10);

        if (clickCount >= 9) {
            navigateButton.hidden = false;
        }
    });

    navigateButton.addEventListener('click', () => {
        location.href = "yum.html"
    })
});
